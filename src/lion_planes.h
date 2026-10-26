/*-------------------------------------------------------------------------
 * lion_planes.h
 *	  Bit-plane arithmetic (DESIGN.md §50), and the one choice of SIMD
 *	  implementation every kernel of the extension follows.
 *
 *	  n values of nbits bits each are nbits bit-planes: plane j holds bit j
 *	  of every value, bit i of its word w being bit j of value 64 w + i.  A
 *	  set of rows F, as a bit vector, then weighs the values it selects as
 *
 *		sum over i in F of x_i = sum over j of 2^j popcount(F AND plane_j)
 *
 *	  which is what lionvec's plane-1 estimate computes, F a row's sign bits
 *	  and the planes the query's four bits a dimension, and what a
 *	  bit-sliced index's sum computes, F a filter's container and the planes
 *	  the column's slices.  The two keep their planes in two layouts, both
 *	  taken here:
 *
 *		INTERLEAVED	planes[w * k + j]: word w of every plane together, as
 *					lionvec's query keeps its four (lion_vec_math.h)
 *		STRIDED		planes[j][w]: each plane its own array, wherever it
 *					lies - a BITSET's or a NARROW's payload, one a slice
 *
 *	  This module counts per plane, one pass over the bits for every plane
 *	  (lion_planes_count_*()); weighs, where the caller knows the sum fits
 *	  (lion_planes_dot_*()); and turns values into planes and back through a
 *	  64 x 64 bit-matrix transpose (lion_planes_scatter_*(),
 *	  lion_planes_gather_*()).
 *
 *	  It also holds the extension's choice among its SIMD implementations
 *	  (portable, POPCNT, AVX2, AVX-512 with VPOPCNTDQ), made once per process
 *	  from what the build and the CPU have, which the container library's
 *	  whole-bitset kernels (lion_container.c) and lionvec's
 *	  (lion_vec_math.c) follow as these kernels do.
 *	  Every implementation returns the same counts, to the bit.
 *
 *	  Frontend-safe (c.h, port/pg_bitutils.h and libpgcommon only), so that
 *	  test/unit/planes_test.c checks it without a server; nothing here
 *	  allocates, and every array is the caller's, of the length its comment
 *	  gives.
 *-------------------------------------------------------------------------
 */
#ifndef LION_PLANES_H
#define LION_PLANES_H

#include "c.h"

#include "port/pg_bitutils.h"

/* ----------------------------------------------------------------
 *					the SIMD implementations
 * ----------------------------------------------------------------
 */

/*
 * The x86-64 implementations: GCC or clang, whose __attribute__((target))
 * compiles a function for AVX2, POPCNT or AVX-512 whatever -march the rest
 * of the extension is built for, and whose __builtin_cpu_supports() says at
 * run time whether it may run (and that the OS saves the registers).  From
 * GCC 8 and clang 8, which know VPOPCNTDQ, the AVX-512 one; from GCC 11 and
 * clang 18, whose __builtin_cpu_supports() knows "f16c", F16C's presence.
 * LION_NO_SIMD (make LION_NO_SIMD=1) leaves them all out, and every build
 * then runs the portable one.  lion_container.c and lion_vec_math.c build
 * their own x86-64 kernels under these tests.
 */
#if !defined(LION_NO_SIMD) && defined(__x86_64__) && \
	((defined(__clang__) && __clang_major__ >= 6) || \
	 (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 5))
#define LION_HAVE_X86_SIMD 1
#define LION_TARGET(isa)	__attribute__((target(isa)))
#if (defined(__clang__) && __clang_major__ >= 8) || \
	(!defined(__clang__) && __GNUC__ >= 8)
#define LION_HAVE_X86_AVX512 1
#endif
#if (defined(__clang__) && __clang_major__ >= 18) || \
	(!defined(__clang__) && __GNUC__ >= 11)
#define LION_HAVE_X86_F16C 1
#endif
#endif

/*
 * The portable kernels' popcount of a word.  __builtin_popcountll() is an
 * instruction where the build's target has one - CNT on aarch64, POPCNT on
 * an x86 build for a CPU that has it (-mpopcnt, -march=native) - and there
 * a portable kernel is a fused loop like the POPCNT one's
 * (LION_POPCOUNT64_INLINE).  On x86 built for the baseline ISA the builtin
 * is a call into libgcc's table lookup, and pg_popcount64() the POPCNT
 * instruction behind a function pointer (before PostgreSQL 19) or a dozen
 * instructions inline (19 on), which is what a portable kernel counts with
 * there (lion_container.c, "whole-bitset kernels").
 */
#if (defined(__GNUC__) || defined(__clang__)) && \
	(defined(__POPCNT__) || !(defined(__x86_64__) || defined(__i386__)))
#define LION_POPCOUNT64_INLINE 1
#define lion_popcount64(w)	((uint32) __builtin_popcountll(w))
#else
#define lion_popcount64(w)	((uint32) pg_popcount64(w))
#endif

/*
 * The implementations, best last.  AUTO is not one: given to
 * lion_simd_force() it means the best this CPU runs, and
 * lion_simd_current() never returns it.
 */
typedef enum LionSimdImpl
{
	LION_SIMD_AUTO = 0,			/* the best this CPU runs */
	LION_SIMD_PORTABLE,			/* plain C */
	LION_SIMD_POPCNT,			/* x86-64: a POPCNT a word */
	LION_SIMD_AVX2,				/* x86-64: AVX2 */
	LION_SIMD_AVX512			/* x86-64: AVX2, popcounts by AVX-512's
								 * VPOPCNTDQ on 256-bit vectors */
} LionSimdImpl;

#define LION_SIMD_NIMPLS	(LION_SIMD_AVX512 + 1)

/* Whether the build has impl and the CPU runs it; AUTO and PORTABLE always. */
extern bool lion_simd_has(LionSimdImpl impl);

/* The best implementation the build has and the CPU runs. */
extern LionSimdImpl lion_simd_best(void);

/*
 * Whether the build knows F16C and the CPU has it: what lionvec converts a
 * halfvec's halves with, beside its AVX2 and AVX-512 kernels (DESIGN.md
 * §44.3, "Kernels").  Not an implementation of its own, and not forced.
 */
extern bool lion_simd_f16c(void);

/* "auto", "portable", "popcnt", "avx2", "avx512" */
extern const char *lion_simd_name(LionSimdImpl impl);

/*
 * The implementation every kernel runs: lion_simd_best(), chosen at the
 * first call, unless a FRONTEND build forced another.  Read it through
 * lion_simd_current(), which a kernel's dispatch inlines; the variable is
 * LION_SIMD_AUTO until the first call resolves it.
 */
extern LionSimdImpl lion_simd_in_use;
extern LionSimdImpl lion_simd_resolve(void);

static inline LionSimdImpl
lion_simd_current(void)
{
	LionSimdImpl impl = lion_simd_in_use;

	if (unlikely(impl == LION_SIMD_AUTO))
		impl = lion_simd_resolve();
	return impl;
}

#ifdef FRONTEND
/*
 * For the unit tests: make every kernel - these, the container library's
 * and lionvec's - run impl, or the best again for LION_SIMD_AUTO.  Returns
 * false, and changes nothing, for one the build or the CPU does not have.
 */
extern bool lion_simd_force(LionSimdImpl impl);
#endif

/* ----------------------------------------------------------------
 *					bit-plane arithmetic
 * ----------------------------------------------------------------
 */

/* The most planes a weighted sum, a scatter or a gather takes. */
#define LION_PLANES_MAX_BITS	64

/*
 * counts[j] = sum over w < nwords of popcount(bits[w] AND plane j's word w),
 * for every j < k, in one pass over bits: INTERLEAVED, plane j's word w is
 * planes[w * k + j], planes holding nwords * k words; STRIDED, it is
 * planes[j][w], each of the k arrays holding nwords words, anywhere.
 * nwords and k may be anything, 0 included; counts holds k.  A count is at
 * most 64 * nwords, so a uint32 holds it below 2^26 words.  Per plane, and
 * not weighed: 64 planes of 512 words weigh to more than 2^64, and the
 * caller weighs as its sum needs (a bit-sliced index's into int128).
 */
extern void lion_planes_count_interleaved(const uint64 *bits, const uint64 *planes,
										  uint32 nwords, uint32 k, uint32 *counts);
extern void lion_planes_count_strided(const uint64 *bits, const uint64 *const *planes,
									  uint32 nwords, uint32 k, uint32 *counts);

/*
 * sum over j < k of 2^j counts[j], counts as above, modulo 2^64: for a
 * caller that knows the sum fits, as it does when (2^k - 1) 64 nwords is
 * below 2^64 - lionvec's, k = 4 over at most 127 words, is at most 121,920.
 * k at most LION_PLANES_MAX_BITS.
 */
extern uint64 lion_planes_dot_interleaved(const uint64 *bits, const uint64 *planes,
										  uint32 nwords, uint32 k);
extern uint64 lion_planes_dot_strided(const uint64 *bits, const uint64 *const *planes,
									  uint32 nwords, uint32 k);

/*
 * The 64 x 64 bit matrix m transposed in place: bit c of word r goes to
 * bit r of word c (Hacker's Delight, 7-3).
 */
extern void lion_planes_transpose64(uint64 *m);

/*
 * Scatter: the n values into nbits planes of nwords = ceil(n / 64) words,
 * bit i of plane j's word w being bit j of values[64 w + i], and 0 for
 * 64 w + i >= n; a value's bits from nbits up are left out.  nbits is 1 ..
 * LION_PLANES_MAX_BITS.  INTERLEAVED, planes holds nwords * nbits words;
 * STRIDED, each of planes[0 .. nbits) holds nwords.
 *
 * Gather, the inverse: values[i], i < n, the value whose bit j is bit i % 64
 * of plane j's word i / 64, for j < nbits.  What the planes hold past
 * value n - 1 is not read as a value.
 */
extern void lion_planes_scatter_interleaved(const uint64 *values, uint32 n,
											uint32 nbits, uint64 *planes);
extern void lion_planes_scatter_strided(const uint64 *values, uint32 n,
										uint32 nbits, uint64 *const *planes);
extern void lion_planes_gather_interleaved(const uint64 *planes, uint32 n,
										   uint32 nbits, uint64 *values);
extern void lion_planes_gather_strided(const uint64 *const *planes, uint32 n,
									   uint32 nbits, uint64 *values);

#endif							/* LION_PLANES_H */
