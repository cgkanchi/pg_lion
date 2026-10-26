/*-------------------------------------------------------------------------
 * lion_vec_math.c
 *	  lionvec's rotation, codes and estimates (lion_vec_math.h, DESIGN.md
 *	  §44.3 - §44.4).
 *
 *	  Nothing here allocates: every array is the caller's, of the length its
 *	  comment gives, so the same code runs in a backend and in
 *	  test/unit/vec_test.c, whose buffers are exact-size heap allocations
 *	  that `make unit SANITIZE=1` checks to the byte.
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#include "miscadmin.h"
#else
#include "c.h"
#endif

#include <math.h>
#include <string.h>

#include "common/pg_prng.h"
#include "port/pg_bitutils.h"

#include "lion_vec_math.h"

#ifndef pg_always_inline
#define pg_always_inline pg_attribute_always_inline
#endif

/*
 * The x86-64 kernels below are built where lion_planes.h's LION_HAVE_X86_SIMD
 * says the compiler can (GCC 5 or clang 6, and not LION_NO_SIMD), and the
 * halves' F16C one where LION_HAVE_X86_F16C says its __builtin_cpu_supports()
 * knows F16C (GCC 11, clang 18) ("Kernels" below).  None is AVX-512's:
 * plane 1's, the only one there was, is lion_planes.c's.
 */
#ifdef LION_HAVE_X86_SIMD
#include <immintrin.h>
#endif

float		lionvec_recon8[256];

static bool lionvec_tables_done = false;

/* ----------------------------------------------------------------
 * The 8-bit codes
 * ----------------------------------------------------------------
 */

/*
 * Code c is the step [-4 + c / 32, -4 + (c + 1) / 32) of a standard Gaussian
 * coordinate, the first and last open to infinity; recon8[c] is the step's
 * middle, exact in a float.  Uniform steps beat steps of equal probability by
 * a factor of eight in squared error: those are as wide in the tails as the
 * tails are long.  A coordinate beyond four standard deviations, one in
 * 16000, is clipped, and alpha8 accounts for that as for any other rounding.
 */
void
lionvec_tables_init(void)
{
	int			k;

	if (lionvec_tables_done)
		return;
	for (k = 0; k < 256; k++)
		lionvec_recon8[k] = -LIONVEC_CLIP8 + ((float) k + 0.5f) / LIONVEC_STEPS8;
	lionvec_tables_done = true;
}

/* The code of z.  A NaN, which no vector pgvector accepts has, gets 0. */
uint8
lionvec_code8(float z)
{
	float		t = (z + LIONVEC_CLIP8) * LIONVEC_STEPS8;

	if (!(t >= 1.0f))
		return 0;
	if (t >= 255.0f)
		return 255;
	return (uint8) t;
}

/* ----------------------------------------------------------------
 * pgvector's datums
 * ----------------------------------------------------------------
 */

int
lionvec_datum_dim(const void *p, Size len, bool half)
{
	const LionVecPgVector *vec = (const LionVecPgVector *) p;
	Size		elem = half ? sizeof(uint16) : sizeof(float);

	StaticAssertStmt(offsetof(LionVecPgVector, x) ==
					 offsetof(LionVecPgHalfVector, x),
					 "a vector's header is a halfvec's");
	if (len < offsetof(LionVecPgVector, x) || vec->dim < 0 ||
		len < offsetof(LionVecPgVector, x) + elem * (Size) vec->dim)
		return -1;
	return vec->dim;
}

/* ----------------------------------------------------------------
 * The rotation
 * ----------------------------------------------------------------
 */

/* Floats in a rotation's signs, the length lionvec_rotation_init() needs. */
Size
lionvec_rotation_nsigns(int dimsp)
{
	int			nwin = (dimsp & (dimsp - 1)) == 0 ? 1 : 2;

	return (Size) LIONVEC_ROUNDS * nwin * dimsp;
}

/*
 * The rotation of dimsp dimensions (a positive multiple of 64) from seed.
 * Each round flips the signs of a pseudo-random half of the coordinates and
 * applies the normalized Walsh-Hadamard transform to each window in turn;
 * when dimsp is not a power of two there are two windows of the largest
 * power of two below it, one at each end, overlapping in the middle, each
 * after its own signs.  Every step is orthogonal, so the whole is.  The
 * signs come from pg_prng, which is the same generator on every platform,
 * so an index's rotation is the same wherever it is read.
 */
void
lionvec_rotation_init(LionVecRotation *rot, int dimsp, uint32 seed,
					  float *signs)
{
	pg_prng_state st;
	Size		n;
	Size		i;
	int			block = 64;

	Assert(dimsp >= 64 && dimsp % 64 == 0);
	while (block * 2 <= dimsp)
		block *= 2;

	rot->dimsp = dimsp;
	rot->block = block;
	rot->nwin = block == dimsp ? 1 : 2;
	rot->win[0] = 0;
	rot->win[1] = dimsp - block;
	rot->signs = signs;

	n = lionvec_rotation_nsigns(dimsp);
	Assert(n == (Size) LIONVEC_ROUNDS * rot->nwin * dimsp);
	pg_prng_seed(&st, (uint64) seed);
	for (i = 0; i < n; i++)
		signs[i] = pg_prng_bool(&st) ? 1.0f : -1.0f;
}

/* The normalized Walsh-Hadamard transform of x[0 .. n), n a power of two. */
static void
lionvec_fwht(float *x, int n)
{
	int			h;
	int			i;
	int			j;
	float		scale = (float) (1.0 / sqrt((double) n));

	for (h = 1; h < n; h *= 2)
	{
		for (i = 0; i < n; i += 2 * h)
		{
			for (j = i; j < i + h; j++)
			{
				float		a = x[j];
				float		b = x[j + h];

				x[j] = a + b;
				x[j + h] = a - b;
			}
		}
	}
	for (i = 0; i < n; i++)
		x[i] *= scale;
}

static void
lionvec_rotate_portable(const LionVecRotation *rot, float *x)
{
	int			r;
	int			w;
	int			i;
	const float *s = rot->signs;

	for (r = 0; r < LIONVEC_ROUNDS; r++)
	{
		for (w = 0; w < rot->nwin; w++)
		{
			for (i = 0; i < rot->dimsp; i++)
				x[i] *= s[i];
			s += rot->dimsp;
			lionvec_fwht(x + rot->win[w], rot->block);
		}
	}
}

/* the rotation's kernel, by the extension's choice ("Kernels" below) */
static void lionvec_rotate_kernel(const LionVecRotation *rot, float *x);

/* Rotate x[0 .. dimsp) in place. */
void
lionvec_rotate(const LionVecRotation *rot, float *x)
{
	lionvec_rotate_kernel(rot, x);
}

/*
 * The vector an index works with of v[0 .. dims): padded to x[0 .. dimsp)
 * and, for cosine, scaled to unit length; then rotated.  False when cosine
 * is given a zero vector, whose distance to anything is NaN, and x is then
 * left as it is.
 */
bool
lionvec_prepare(const LionVecRotation *rot, int metric, const float *v,
				int dims, float *x)
{
	int			i;

	Assert(dims >= 0 && dims <= rot->dimsp);
	if (metric == LIONVEC_COSINE)
	{
		double		n2 = 0.0;
		double		inv;

		for (i = 0; i < dims; i++)
			n2 += (double) v[i] * (double) v[i];
		if (!(n2 > 0.0))
			return false;
		inv = 1.0 / sqrt(n2);
		for (i = 0; i < dims; i++)
			x[i] = (float) (v[i] * inv);
	}
	else
		memcpy(x, v, sizeof(float) * dims);
	for (i = dims; i < rot->dimsp; i++)
		x[i] = 0.0f;
	lionvec_rotate(rot, x);
	return true;
}

/* ----------------------------------------------------------------
 * A row's codes
 * ----------------------------------------------------------------
 */

/*
 * Centre the rotated x[0 .. dimsp) on mean (NULL: zero) in place and encode
 * it into e, whose bits must hold dimsp / 64 words and codes dimsp bytes.  A
 * row at the mean has no direction: its codes are all zero bits and the
 * middle bin, with alphas of 1, which makes every estimate of it 0 with an
 * error of 0 - exactly right, as its norm is 0.
 */
void
lionvec_encode(const float *mean, int dimsp, float *x, LionVecEncoded *e)
{
	double		n2 = 0.0;
	double		xm = 0.0;
	double		s1 = 0.0;
	double		rr = 0.0;
	double		rx = 0.0;
	double		norm;
	double		scale;
	uint32		pop = 0;
	int			i;

	Assert(dimsp >= 64 && dimsp % 64 == 0);
	if (mean != NULL)
	{
		for (i = 0; i < dimsp; i++)
		{
			x[i] -= mean[i];
			xm += (double) x[i] * (double) mean[i];
		}
	}
	for (i = 0; i < dimsp; i++)
		n2 += (double) x[i] * (double) x[i];
	norm = sqrt(n2);

	memset(e->bits, 0, sizeof(uint64) * (dimsp / 64));
	e->norm = (float4) norm;
	e->xm = (float4) xm;
	if (!(norm > 0.0))
	{
		memset(e->codes, 128, dimsp);
		e->alpha1 = 1.0f;
		e->alpha8 = 1.0f;
		e->rnorm8 = 1.0f;
		e->popcount = 0;
		return;
	}

	scale = sqrt((double) dimsp) / norm;
	for (i = 0; i < dimsp; i++)
	{
		double		xi = x[i];
		uint8		c = lionvec_code8((float) (xi * scale));
		double		r = lionvec_recon8[c];

		if (xi > 0.0)
		{
			e->bits[i / 64] |= UINT64CONST(1) << (i % 64);
			pop++;
		}
		s1 += fabs(xi);
		e->codes[i] = c;
		rr += r * r;
		rx += r * xi;
	}
	e->popcount = (uint16) pop;
	e->alpha1 = (float4) (s1 / (norm * sqrt((double) dimsp)));
	e->rnorm8 = (float4) sqrt(rr);
	e->alpha8 = (float4) (rx / (sqrt(rr) * norm));
}

/* ----------------------------------------------------------------
 * A query
 * ----------------------------------------------------------------
 */

/*
 * Prepare q from the rotated query x[0 .. dimsp): q->qc must hold dimsp
 * floats and q->planes dimsp / 64 * LIONVEC_QBITS words, and q->dimsp be
 * set.  The quantization is uniform over qc's own range, rounded to the
 * nearest step; its variance in an estimate is what its errors actually are,
 * |e|^2 / dimsp, taken over a row's signs as if they were coin flips.
 *
 * The sums are kept in locals and each word of the planes is assembled in
 * registers, its bits set without a branch: the build runs this for every
 * sample row in every iteration of k-means, and accumulating into q's own
 * fields, which the compiler must assume the stores to qc and planes may
 * overwrite, and a branch on each bit of each code, which the CPU cannot
 * predict, made it 16% of the build (45 us a row at 1536 dimensions, 12
 * now).  The arithmetic is the same, in the same order.
 */
void
lionvec_query_init(LionVecQuery *q, int metric, const float *mean,
				   const float *x, double eps)
{
	int			dimsp = q->dimsp;
	int			levels = (1 << LIONVEC_QBITS) - 1;
	double		mn = INFINITY;
	double		mx = -INFINITY;
	double		mq = 0.0;
	double		e2 = 0.0;
	double		qnorm2 = 0.0;
	double		qsum = 0.0;
	double		qlo;
	double		qdelta;
	int			i;
	int			w;

	Assert(dimsp >= 64 && dimsp % 64 == 0);
	for (i = 0; i < dimsp; i++)
	{
		double		m = mean != NULL ? mean[i] : 0.0;
		float		c = (float) (x[i] - m);

		q->qc[i] = c;
		mq += m * x[i];
		qnorm2 += (double) c * (double) c;
		mn = Min(mn, (double) c);
		mx = Max(mx, (double) c);
	}

	qlo = mn;
	qdelta = mx > mn ? (mx - mn) / levels : 1.0;
	StaticAssertStmt(LIONVEC_QBITS == 4, "the planes are assembled four at a time");
	for (w = 0; w < dimsp / 64; w++)
	{
		uint64		p0 = 0;
		uint64		p1 = 0;
		uint64		p2 = 0;
		uint64		p3 = 0;
		int			b;

		for (b = 0; b < 64; b++)
		{
			double		c = q->qc[w * 64 + b];
			double		t = (c - qlo) / qdelta + 0.5;
			int			u = t >= levels ? levels : (t > 0.0 ? (int) t : 0);
			double		qq = qlo + qdelta * u;

			p0 |= (uint64) (u & 1) << b;
			p1 |= (uint64) ((u >> 1) & 1) << b;
			p2 |= (uint64) ((u >> 2) & 1) << b;
			p3 |= (uint64) ((u >> 3) & 1) << b;
			qsum += qq;
			e2 += (c - qq) * (c - qq);
		}
		q->planes[w * LIONVEC_QBITS] = p0;
		q->planes[w * LIONVEC_QBITS + 1] = p1;
		q->planes[w * LIONVEC_QBITS + 2] = p2;
		q->planes[w * LIONVEC_QBITS + 3] = p3;
	}

	q->qnorm2 = qnorm2;
	q->qlo = qlo;
	q->qdelta = qdelta;
	q->qsum = qsum;
	q->var1 = q->qnorm2 / (dimsp - 1);
	q->varq = e2 / dimsp;
	q->eps = eps;
	if (metric == LIONVEC_L2)
		q->offset = q->qnorm2;
	else if (metric == LIONVEC_IP)
		q->offset = mq;
	else
		q->offset = mq - 1.0;
}

/* ----------------------------------------------------------------
 * Kernels
 *
 * What a scan and a build spend their arithmetic on, at 1536 dimensions
 * (DESIGN.md §44.3, "Kernels"): plane 1's popcounts against the query's
 * planes, for every centroid of every query and every row a probe reads,
 * and, in k-means, every centroid for every sample row in every iteration;
 * the 8-bit codes' sum, for every centroid and row a scan refines; the
 * exact distances of k-means' candidates; and the rotation and, of a
 * halfvec, the conversion of its halves to floats, each once a query and
 * twice a row built.  Each has a portable kernel and, on x86-64, ones
 * compiled with __attribute__((target)) for what the CPU has, so that the
 * extension builds with the default -march and runs on any x86-64.  Which
 * runs is the extension's one choice of implementation, lion_simd_current()
 * (lion_planes.h, DESIGN.md §50), resolved at the first call to the best
 * the CPU runs and read at every call, so that a FRONTEND build's
 * lion_simd_force() - lionvec_kernel_force() - moves every kernel at once:
 *
 *	POPCNT	plane 1 a word at a time with the POPCNT instruction; the rest
 *			portable
 *	AVX2	plane 1 a word's four planes in one vector against the row's
 *			word broadcast, each byte's popcount looked up a nibble at a
 *			time with VPSHUFB (Mula); the sums and the rotation four
 *			doubles or eight floats at a time; and, where the CPU has F16C
 *			as well, which AVX2 does not imply (lion_simd_f16c()), the
 *			halves eight at a time with VCVTPH2PS
 *	AVX-512	AVX2's, but plane 1 counted with VPOPCNTDQ, a word's four
 *			planes in one instruction
 *
 * Plane 1's kernels are lion_planes.c's, the weighted sum of four
 * interleaved planes (lion_planes_dot_interleaved(), whose k = 4 has a pass
 * of its own in each implementation); the others are here.  AVX-512 is
 * taken for VPOPCNTDQ alone (Ice Lake, Zen 4 and on), on 256-bit vectors:
 * AVX-512BW's VPSHUFB counted plane 1 only a sixth faster than AVX2's, and
 * the sums eight doubles at a time only a tenth faster, where a scan waits
 * on the codes' cache lines more than on either, so no 512-bit instruction
 * runs, for which some cores lower their clock; its set's other kernels are
 * AVX2's.
 *
 * Every kernel returns what the portable one does, to the bit, so that a
 * scan, a build and an insert decide alike whichever runs.  Plane 1's are
 * integer counts.  The sums are in double, of terms each kernel computes
 * with the same operations - the 8-bit codes' products are exact, a code's
 * reconstruction being a float and a float's product with a float a double
 * - added in LIONVEC_LANES partial sums, the i-th term into partial i %
 * LIONVEC_LANES, and those in a fixed tree (lionvec_lanes_sum()): the
 * portable kernels add in that order too.  The file is compiled with
 * -ffp-contract=off (the Makefile), for GCC and clang would otherwise fuse
 * a multiply and an add into one rounding wherever the target has fused
 * multiply-adds - the portable kernels built for aarch64 or x86-64-v3 -
 * and l2sq's terms, unlike the 8-bit codes', are not exact.  The
 * rotation's butterflies are the portable ones' additions and subtractions,
 * of the same operands, in the same order for each coordinate.  The halves'
 * conversion is exact, every half being a float, and VCVTPH2PS makes a NaN
 * the quiet NaN of its payload, as the portable kernel does, whatever
 * MXCSR's DAZ and FTZ say.
 * ----------------------------------------------------------------
 */

/* ---------- portable ---------- */

/*
 * The partial sums p[0 .. LIONVEC_LANES) added up: (p_l + p_l+8) + (p_l+4 +
 * p_l+12) for each l < 4, then (u_0 + u_2) + (u_1 + u_3) of those - the
 * tree the vector kernels fold their lanes in.
 */
static inline double
lionvec_lanes_sum(const double *p)
{
	double		u[4];
	int			l;

	StaticAssertStmt(LIONVEC_LANES == 16, "the tree is of sixteen lanes");
	for (l = 0; l < 4; l++)
		u[l] = (p[l] + p[l + 8]) + (p[l + 4] + p[l + 12]);
	return (u[0] + u[2]) + (u[1] + u[3]);
}

/*
 * Code c's reconstruction, lionvec_recon8[c], computed: c / 32 is exact, and
 * so is the sum, a multiple of 1/64 under 4 (the unit test checks every
 * code), so that it is the same double whether a multiply and an add or a
 * fused multiply-add computes it, as is its product with a float.  The
 * kernels compute rather than look up, which vectorizes.
 */
#define LIONVEC_RECON8(c) \
	((double) (c) * (1.0 / LIONVEC_STEPS8) + (-LIONVEC_CLIP8 + 0.5 / LIONVEC_STEPS8))

/*
 * The portable sums index each run of LIONVEC_LANES from its own pointer:
 * under PostgreSQL's -fwrapv, i + l might wrap, and GCC then loads the
 * run's terms one by one as if gathered, at twice the time.
 */
static double
lionvec_sum8_portable(const uint8 *codes, const float *qc, int dimsp)
{
	double		p[LIONVEC_LANES] = {0};
	int			i;
	int			l;

	Assert(dimsp % LIONVEC_LANES == 0);
	for (i = 0; i < dimsp; i += LIONVEC_LANES)
	{
		const uint8 *c = codes + i;
		const float *q = qc + i;

		for (l = 0; l < LIONVEC_LANES; l++)
			p[l] += LIONVEC_RECON8(c[l]) * (double) q[l];
	}
	return lionvec_lanes_sum(p);
}

static double
lionvec_l2sq_portable(const float *a, const float *b, int dimsp)
{
	double		p[LIONVEC_LANES] = {0};
	int			i;
	int			l;

	Assert(dimsp % LIONVEC_LANES == 0);
	for (i = 0; i < dimsp; i += LIONVEC_LANES)
	{
		const float *x = a + i;
		const float *y = b + i;

		for (l = 0; l < LIONVEC_LANES; l++)
		{
			double		d = (double) x[l] - (double) y[l];

			p[l] += d * d;
		}
	}
	return lionvec_lanes_sum(p);
}

/*
 * The float of the half h, exactly, from its bits alone, so that no mode of
 * the FPU can change it: a normal's exponent rebiased from 15 to 127; a
 * subnormal, m 2^-24, normalized - m's leading bit, at bit lead, shifted to
 * bit 23, where adding it carries into the exponent, of lead - 24; a zero
 * signed; and infinity, or a NaN made quiet with its payload kept, at the
 * float's exponent of all ones.
 */
static inline float
lionvec_half_float(uint16 h)
{
	uint32		em = h & 0x7fff;	/* the exponent and mantissa */
	uint32		bits;
	float		f;

	if (em >= 0x7c00)
		bits = 0x7f800000 | ((em & 0x3ff) << 13) | (em > 0x7c00 ? 0x00400000 : 0);
	else if (em >= 0x0400)
		bits = (em << 13) + ((uint32) (127 - 15) << 23);
	else if (em != 0)
	{
		int			lead = pg_leftmost_one_pos32(em);	/* 0 .. 9 */

		bits = ((uint32) (lead - 24 + 127 - 1) << 23) + (em << (23 - lead));
	}
	else
		bits = 0;
	bits |= (uint32) (h & 0x8000) << 16;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

static void
lionvec_halves_portable(const uint16 *h, int n, float *x)
{
	int			i;

	for (i = 0; i < n; i++)
		x[i] = lionvec_half_float(h[i]);
}

#ifdef LION_HAVE_X86_SIMD

/* ---------- AVX2 ---------- */

/* 16 partial sums in four vectors, added up as lionvec_lanes_sum() does */
static pg_always_inline LION_TARGET("avx2") double
lionvec_avx2_fold(__m256d p0, __m256d p4, __m256d p8, __m256d p12)
{
	__m256d		u = _mm256_add_pd(_mm256_add_pd(p0, p8), _mm256_add_pd(p4, p12));
	__m128d		v = _mm_add_pd(_mm256_castpd256_pd128(u),
							   _mm256_extractf128_pd(u, 1));

	return _mm_cvtsd_f64(_mm_add_sd(v, _mm_unpackhi_pd(v, v)));
}

/*
 * Terms i .. i + 3 of the 8-bit codes' sum: the codes' reconstructions,
 * -4 + (c + 1/2) / 32, computed as lionvec_recon8[] holds them (exactly:
 * the unit test checks it), times qc's.
 */
static pg_always_inline LION_TARGET("avx2") __m256d
lionvec_avx2_terms8(const uint8 *codes, const float *qc)
{
	const __m256d step = _mm256_set1_pd(1.0 / LIONVEC_STEPS8);
	const __m256d first = _mm256_set1_pd(-LIONVEC_CLIP8 + 0.5 / LIONVEC_STEPS8);
	int32		four;
	__m256d		r;

	memcpy(&four, codes, sizeof(four));
	r = _mm256_cvtepi32_pd(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(four)));
	r = _mm256_add_pd(_mm256_mul_pd(r, step), first);
	return _mm256_mul_pd(r, _mm256_cvtps_pd(_mm_loadu_ps(qc)));
}

static LION_TARGET("avx2") double
lionvec_sum8_avx2(const uint8 *codes, const float *qc, int dimsp)
{
	__m256d		p0 = _mm256_setzero_pd();
	__m256d		p4 = _mm256_setzero_pd();
	__m256d		p8 = _mm256_setzero_pd();
	__m256d		p12 = _mm256_setzero_pd();
	int			i;

	Assert(dimsp % LIONVEC_LANES == 0);
	for (i = 0; i < dimsp; i += LIONVEC_LANES)
	{
		p0 = _mm256_add_pd(p0, lionvec_avx2_terms8(codes + i, qc + i));
		p4 = _mm256_add_pd(p4, lionvec_avx2_terms8(codes + i + 4, qc + i + 4));
		p8 = _mm256_add_pd(p8, lionvec_avx2_terms8(codes + i + 8, qc + i + 8));
		p12 = _mm256_add_pd(p12, lionvec_avx2_terms8(codes + i + 12, qc + i + 12));
	}
	return lionvec_avx2_fold(p0, p4, p8, p12);
}

/* terms i .. i + 3 of l2sq */
static pg_always_inline LION_TARGET("avx2") __m256d
lionvec_avx2_terms_l2(const float *a, const float *b)
{
	__m256d		d = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a)),
								  _mm256_cvtps_pd(_mm_loadu_ps(b)));

	return _mm256_mul_pd(d, d);
}

static LION_TARGET("avx2") double
lionvec_l2sq_avx2(const float *a, const float *b, int dimsp)
{
	__m256d		p0 = _mm256_setzero_pd();
	__m256d		p4 = _mm256_setzero_pd();
	__m256d		p8 = _mm256_setzero_pd();
	__m256d		p12 = _mm256_setzero_pd();
	int			i;

	Assert(dimsp % LIONVEC_LANES == 0);
	for (i = 0; i < dimsp; i += LIONVEC_LANES)
	{
		p0 = _mm256_add_pd(p0, lionvec_avx2_terms_l2(a + i, b + i));
		p4 = _mm256_add_pd(p4, lionvec_avx2_terms_l2(a + i + 4, b + i + 4));
		p8 = _mm256_add_pd(p8, lionvec_avx2_terms_l2(a + i + 8, b + i + 8));
		p12 = _mm256_add_pd(p12, lionvec_avx2_terms_l2(a + i + 12, b + i + 12));
	}
	return lionvec_avx2_fold(p0, p4, p8, p12);
}

/*
 * lionvec_fwht() eight floats at a time: the butterflies of strides 1, 2
 * and 4 inside each run of eight, by swapping the run's halves, pairs or
 * neighbours and taking a + b where the portable loop does and a - b where
 * it does (the swapped vector less the run); then those of 8 and up
 * between runs, and the scale.  n is a power of two, 64 or more.
 */
static LION_TARGET("avx2") void
lionvec_fwht_avx2(float *x, int n)
{
	const __m256 scale = _mm256_set1_ps((float) (1.0 / sqrt((double) n)));
	int			h;
	int			i;
	int			j;

	for (i = 0; i < n; i += 8)
	{
		__m256		v = _mm256_loadu_ps(x + i);
		__m256		s;

		s = _mm256_permute_ps(v, 0xB1); /* 1 0 3 2 */
		v = _mm256_blend_ps(_mm256_add_ps(v, s), _mm256_sub_ps(s, v), 0xAA);
		s = _mm256_permute_ps(v, 0x4E); /* 2 3 0 1 */
		v = _mm256_blend_ps(_mm256_add_ps(v, s), _mm256_sub_ps(s, v), 0xCC);
		s = _mm256_permute2f128_ps(v, v, 0x01); /* 4 5 6 7 0 1 2 3 */
		v = _mm256_blend_ps(_mm256_add_ps(v, s), _mm256_sub_ps(s, v), 0xF0);
		_mm256_storeu_ps(x + i, v);
	}
	for (h = 8; h < n; h *= 2)
	{
		for (i = 0; i < n; i += 2 * h)
		{
			for (j = i; j < i + h; j += 8)
			{
				__m256		a = _mm256_loadu_ps(x + j);
				__m256		b = _mm256_loadu_ps(x + j + h);

				_mm256_storeu_ps(x + j, _mm256_add_ps(a, b));
				_mm256_storeu_ps(x + j + h, _mm256_sub_ps(a, b));
			}
		}
	}
	for (i = 0; i < n; i += 8)
		_mm256_storeu_ps(x + i, _mm256_mul_ps(_mm256_loadu_ps(x + i), scale));
}

static LION_TARGET("avx2") void
lionvec_rotate_avx2(const LionVecRotation *rot, float *x)
{
	int			r;
	int			w;
	int			i;
	const float *s = rot->signs;

	for (r = 0; r < LIONVEC_ROUNDS; r++)
	{
		for (w = 0; w < rot->nwin; w++)
		{
			for (i = 0; i < rot->dimsp; i += 8)
				_mm256_storeu_ps(x + i, _mm256_mul_ps(_mm256_loadu_ps(x + i),
													  _mm256_loadu_ps(s + i)));
			s += rot->dimsp;
			lionvec_fwht_avx2(x + rot->win[w], rot->block);
		}
	}
}

#ifdef LION_HAVE_X86_F16C

/* ---------- F16C ---------- */

/* The halves eight at a time, the last n % 8 from a vector padded with zeros. */
static LION_TARGET("avx2,f16c") void
lionvec_halves_f16c(const uint16 *h, int n, float *x)
{
	int			i;

	for (i = 0; i + 8 <= n; i += 8)
		_mm256_storeu_ps(x + i, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *) (h + i))));
	if (i < n)
	{
		uint16		last[8] = {0};
		float		out[8];

		memcpy(last, h + i, sizeof(uint16) * (n - i));
		_mm256_storeu_ps(out, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *) last)));
		memcpy(x + i, out, sizeof(float) * (n - i));
	}
}

#endif							/* LION_HAVE_X86_F16C */

#endif							/* LION_HAVE_X86_SIMD */

/* ---------- the choice ---------- */

#ifdef LION_HAVE_X86_SIMD
/*
 * Whether impl's set has the AVX2 kernels: AVX2's, and AVX-512's, whose one
 * kernel of its own, plane 1's, is lion_planes.c's.
 */
static inline bool
lionvec_avx2_set(LionSimdImpl impl)
{
	return impl == LION_SIMD_AVX2 || impl == LION_SIMD_AVX512;
}
#endif

/*
 * Whether the build knows F16C and the CPU has it (lion_simd_f16c()), asked
 * once: the AVX2 and AVX-512 sets convert halves with it where it is, and
 * portably where it is not.  Not forced, being no implementation of its own.
 */
static int	lionvec_f16c = -1;

static inline bool
lionvec_has_f16c(void)
{
	if (unlikely(lionvec_f16c < 0))
		lionvec_f16c = lion_simd_f16c() ? 1 : 0;
	return lionvec_f16c > 0;
}

double
lionvec_sum8(const uint8 *codes, const float *qc, int dimsp)
{
#ifdef LION_HAVE_X86_SIMD
	if (lionvec_avx2_set(lion_simd_current()))
		return lionvec_sum8_avx2(codes, qc, dimsp);
#endif
	return lionvec_sum8_portable(codes, qc, dimsp);
}

double
lionvec_l2sq(const float *a, const float *b, int dimsp)
{
#ifdef LION_HAVE_X86_SIMD
	if (lionvec_avx2_set(lion_simd_current()))
		return lionvec_l2sq_avx2(a, b, dimsp);
#endif
	return lionvec_l2sq_portable(a, b, dimsp);
}

void
lionvec_halves(const uint16 *h, int n, float *x)
{
#ifdef LION_HAVE_X86_F16C
	if (lionvec_avx2_set(lion_simd_current()) && lionvec_has_f16c())
	{
		lionvec_halves_f16c(h, n, x);
		return;
	}
#endif
	lionvec_halves_portable(h, n, x);
}

static void
lionvec_rotate_kernel(const LionVecRotation *rot, float *x)
{
#ifdef LION_HAVE_X86_SIMD
	if (lionvec_avx2_set(lion_simd_current()))
	{
		lionvec_rotate_avx2(rot, x);
		return;
	}
#endif
	lionvec_rotate_portable(rot, x);
}

/*
 * Called once at load, before any vector is rotated: the choice resolved
 * and F16C looked for, so that no scan pays for either.  In a FRONTEND
 * build, the best set again after lionvec_kernel_force().
 */
void
lionvec_kernels_init(void)
{
#ifdef FRONTEND
	(void) lion_simd_force(LION_SIMD_AUTO);
#endif
	(void) lion_simd_current();
	(void) lionvec_has_f16c();
}

#ifdef FRONTEND
bool
lionvec_kernel_force(int which)
{
	StaticAssertStmt(LIONVEC_KERNELS_PORTABLE == 0 &&
					 LION_SIMD_PORTABLE + LIONVEC_KERNELS_POPCNT == LION_SIMD_POPCNT &&
					 LION_SIMD_PORTABLE + LIONVEC_KERNELS_AVX2 == LION_SIMD_AVX2 &&
					 LION_SIMD_PORTABLE + LIONVEC_KERNELS_AVX512 == LION_SIMD_AVX512 &&
					 LIONVEC_KERNELS_MAX == LIONVEC_KERNELS_AVX512 &&
					 LION_SIMD_AVX512 == LION_SIMD_NIMPLS - 1,
					 "lionvec's sets are lion_planes.h's implementations, numbered from 0");
	if (which < LIONVEC_KERNELS_PORTABLE || which > LIONVEC_KERNELS_MAX)
		return false;
	return lion_simd_force((LionSimdImpl) (LION_SIMD_PORTABLE + which));
}

bool
lionvec_kernel_f16c(void)
{
#ifdef LION_HAVE_X86_F16C
	return lionvec_avx2_set(lion_simd_current()) && lionvec_has_f16c();
#else
	return false;
#endif
}
#endif

/* ----------------------------------------------------------------
 * Partitions: the nearest centroid, and k-means
 * ----------------------------------------------------------------
 */

int
lionvec_candidates(const LionVecCentroids *c, const LionVecQuery *q,
				   double *lo, double *hi, int *cand)
{
	return lionvec_candidates_n(c, q, 1, lo, hi, cand);
}

void
lionvec_cands_begin(LionVecCandScan *cs, int m)
{
	Assert(m >= 1 && m <= LIONVEC_RANK_MAX);
	cs->m = Max(1, Min(m, LIONVEC_RANK_MAX));
	cs->nleast = 0;
}

bool
lionvec_cands_add(LionVecCandScan *cs, const LionVecQuery *q,
				  const uint64 *bits, uint16 popcount, float norm,
				  float alpha1, double *lo, double *hi)
{
	int			m = cs->m;
	double		est;
	double		err;
	int			i;

	lionvec_estimate1(q, bits, popcount, norm, alpha1, &est, &err);
	lionvec_key_bounds(LIONVEC_L2, norm, 0.0, est, err, lo, hi);
	/* NaN bounds come back infinite, so every comparison here is total */
	if (cs->nleast < m || *hi < cs->least[m - 1])
	{
		i = cs->nleast < m ? cs->nleast++ : m - 1;
		for (; i > 0 && cs->least[i - 1] > *hi; i--)
			cs->least[i] = cs->least[i - 1];
		cs->least[i] = *hi;
	}
	return *lo <= lionvec_cands_threshold(cs);
}

double
lionvec_cands_threshold(const LionVecCandScan *cs)
{
	/* fewer centroids than m: every one can be among the m nearest */
	return cs->nleast == cs->m ? cs->least[cs->m - 1] : INFINITY;
}

int
lionvec_candidates_n(const LionVecCentroids *c, const LionVecQuery *q, int m,
					 double *lo, double *hi, int *cand)
{
	return lionvec_candidates_among(c, NULL, c->k, q, m, lo, hi, cand);
}

int
lionvec_candidates_among(const LionVecCentroids *c, const int *idx, int n,
						 const LionVecQuery *q, int m, double *lo, double *hi,
						 int *cand)
{
	int			nw = c->dimsp / 64;
	LionVecCandScan cs;
	double		thr;
	int			nc = 0;
	int			i;

	Assert(c->dimsp == q->dimsp);
	Assert(n >= 0 && n <= c->k && (idx != NULL || n == c->k));
	lionvec_cands_begin(&cs, m);
	for (i = 0; i < n; i++)
	{
		int			j = idx != NULL ? idx[i] : i;

		Assert(j >= 0 && j < c->k);
		(void) lionvec_cands_add(&cs, q, c->bits + (Size) j * nw,
								 c->popcount[j], c->norm[j], c->alpha1[j],
								 &lo[i], &hi[i]);
	}
	thr = lionvec_cands_threshold(&cs);
	for (i = 0; i < n; i++)
	{
		if (lo[i] <= thr)
			cand[nc++] = idx != NULL ? idx[i] : i;
	}
	/* at least min(n, m) unless a bound came out with lo > hi (the header) */
	return nc;
}

int
lionvec_rank(const double *key, int n, int m, int *order)
{
	int			r = 0;
	int			j;

	if (m <= 0)
		return 0;
	for (j = 0; j < n; j++)
	{
		double		kj = isnan(key[j]) ? INFINITY : key[j];
		int			i;

		if (r == m)
		{
			double		last = isnan(key[order[m - 1]]) ? INFINITY : key[order[m - 1]];

			/* not less than the m-th: ties stay with the lower place */
			if (!(kj < last))
				continue;
			i = m - 1;
		}
		else
			i = r++;
		for (; i > 0; i--)
		{
			double		ki = isnan(key[order[i - 1]]) ? INFINITY : key[order[i - 1]];

			if (!(ki > kj))
				break;
			order[i] = order[i - 1];
		}
		order[i] = j;
	}
	return r;
}

/* place a of key comes after place b: by key, a NaN as infinite, then place */
static inline bool
lionvec_least_after(const double *key, int a, int b)
{
	double		ka = isnan(key[a]) ? INFINITY : key[a];
	double		kb = isnan(key[b]) ? INFINITY : key[b];

	return ka > kb || (ka == kb && a > b);
}

int
lionvec_least(const double *key, int n, int m, int *idx)
{
	int			r = 0;
	int			j;

	if (m <= 0)
		return 0;
	for (j = 0; j < n; j++)
	{
		int			i;

		if (r < m)
		{
			/* up the max-heap from the end */
			for (i = r++; i > 0 && lionvec_least_after(key, j, idx[(i - 1) / 2]);
				 i = (i - 1) / 2)
				idx[i] = idx[(i - 1) / 2];
			idx[i] = j;
			continue;
		}
		/* not before the greatest kept, a later place: it stays out */
		if (lionvec_least_after(key, j, idx[0]))
			continue;
		/* down from the root */
		i = 0;
		for (;;)
		{
			int			c = 2 * i + 1;

			if (c >= r)
				break;
			if (c + 1 < r && lionvec_least_after(key, idx[c + 1], idx[c]))
				c++;
			if (!lionvec_least_after(key, idx[c], j))
				break;
			idx[i] = idx[c];
			i = c;
		}
		idx[i] = j;
	}
	return r;
}

void
lionvec_least_order(const double *key, int *idx, int m)
{
	int			i;

	for (i = 1; i < m; i++)
	{
		int			x = idx[i];
		int			j;

		for (j = i; j > 0 && lionvec_least_after(key, idx[j - 1], x); j--)
			idx[j] = idx[j - 1];
		idx[j] = x;
	}
}

/* ----------------------------------------------------------------
 * Vectors in TOAST: the places of a value's chunks (DESIGN.md §44.13)
 * ----------------------------------------------------------------
 */

int
lionvec_loc_runs(const uint32 *blks, const uint16 *offs, int n,
				 LionVecLocRun *runs, int maxruns)
{
	int			r = 0;
	int			i;

	if (n <= 0 || maxruns <= 0)
		return -1;
	for (i = 0; i < n; i++)
	{
		/* the next line pointer of the same block goes on the run */
		if (r > 0 && runs[r - 1].blk == blks[i] &&
			(uint32) runs[r - 1].off + runs[r - 1].n == offs[i] &&
			runs[r - 1].n < PG_UINT16_MAX)
		{
			runs[r - 1].n++;
			continue;
		}
		if (r >= maxruns)
			return -1;
		runs[r].blk = blks[i];
		runs[r].off = offs[i];
		runs[r].n = 1;
		r++;
	}
	return r;
}

bool
lionvec_loc_runs_ok(const LionVecLocRun *runs, int nruns, int nchunks,
					int maxoff)
{
	int64		sum = 0;
	int			i;

	if (nruns < 1 || nruns > nchunks || nchunks < 1)
		return false;
	for (i = 0; i < nruns; i++)
	{
		if (runs[i].blk == PG_UINT32_MAX || runs[i].n < 1 || runs[i].off < 1 ||
			(int) runs[i].off + (int) runs[i].n - 1 > maxoff)
			return false;
		sum += runs[i].n;
	}
	return sum == nchunks;
}

/* Whether one of the nm sorted blocks lies in [lo, hi]: the first at lo or past. */
static bool
lionvec_blocks_hit(const uint32 *mblks, int nm, uint32 lo, uint32 hi)
{
	int			a = 0;
	int			b = nm;

	if (lo > hi)
		return false;
	while (a < b)
	{
		int			m = a + (b - a) / 2;

		if (mblks[m] < lo)
			a = m + 1;
		else
			b = m;
	}
	return a < nm && mblks[a] <= hi;
}

static int
lionvec_u32_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a;
	uint32		y = *(const uint32 *) b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

int
lionvec_loc_select(const uint32 *page, const uint32 *lo, const uint32 *hi,
				   int n, const uint32 *mblks, int nm, uint32 *out)
{
	int			k = 0;
	int			i;
	int			j;

	for (i = 0; i < n; i++)
		if (lionvec_blocks_hit(mblks, nm, lo[i], hi[i]))
			out[k++] = page[i];
	/* the build's come in order; the inserts' after, and a page may repeat */
	qsort(out, k, sizeof(uint32), lionvec_u32_cmp);
	for (i = j = 0; i < k; i++)
		if (j == 0 || out[j - 1] != out[i])
			out[j++] = out[i];
	return j;
}

/* the SOAR cost of c, given r = x - c1 and |r|^2 */
static double
lionvec_soar_cost_r(const float *x, const float *c1, double r2, const float *c,
					int dimsp, double lambda)
{
	double		d2 = 0.0;
	double		rd = 0.0;
	double		cost;
	int			i;

	for (i = 0; i < dimsp; i++)
	{
		double		d = (double) x[i] - (double) c[i];

		d2 += d * d;
		rd += ((double) x[i] - (double) c1[i]) * d;
	}
	cost = r2 > 0.0 ? d2 + lambda * rd * rd / r2 : d2;
	return isnan(cost) ? INFINITY : cost;
}

static double
lionvec_residual2(const float *x, const float *c1, int dimsp)
{
	double		r2 = 0.0;
	int			i;

	for (i = 0; i < dimsp; i++)
	{
		double		r = (double) x[i] - (double) c1[i];

		r2 += r * r;
	}
	return r2;
}

double
lionvec_soar_cost(const float *x, const float *c1, const float *c, int dimsp,
				  double lambda)
{
	return lionvec_soar_cost_r(x, c1, lionvec_residual2(x, c1, dimsp), c, dimsp,
							   lambda);
}

int
lionvec_soar_second(const float *x, const float *c1, const float *const *cands,
					int n, int dimsp, double lambda)
{
	double		r2 = lionvec_residual2(x, c1, dimsp);
	double		best = INFINITY;
	int			besti = -1;
	int			i;

	for (i = 0; i < n; i++)
	{
		double		cost = lionvec_soar_cost_r(x, c1, r2, cands[i], dimsp, lambda);

		if (besti < 0 || cost < best)
		{
			besti = i;
			best = cost;
		}
	}
	return besti;
}

void
lionvec_decode8(const uint8 *codes, int dimsp, float norm, float alpha8,
				float rnorm8, float *x)
{
	double		scale = rnorm8 > 0.0f ? (double) norm * alpha8 / rnorm8 : 0.0;
	int			i;

	for (i = 0; i < dimsp; i++)
		x[i] = (float) (scale * lionvec_recon8[codes[i]]);
}

void
lionvec_sample_row(const LionVecSample *s, int i, float *x)
{
	const uint8 *codes = s->codes + (Size) i * s->dimsp;
	float		scale = s->scale[i];
	int			j;

	Assert(i >= 0 && i < s->n);
	for (j = 0; j < s->dimsp; j++)
		x[j] = scale * lionvec_recon8[codes[j]];
}

static void
lionvec_unit(float *x, int n)
{
	double		n2 = 0.0;
	int			i;

	for (i = 0; i < n; i++)
		n2 += (double) x[i] * (double) x[i];
	if (n2 > 0.0)
	{
		double		inv = 1.0 / sqrt(n2);

		for (i = 0; i < n; i++)
			x[i] = (float) (x[i] * inv);
	}
}

/*
 * k-means is O(n sqrt(k)) an iteration over up to 2^24 sample rows and
 * 262,144 centroids, and its regions' k-means O(k sqrt(k)): a cancel, a
 * statement_timeout or a shutdown must reach it in the loops over rows and
 * over centroids, not after the build.  The unit tests have no interrupts to
 * check.
 */
#ifndef FRONTEND
#define LIONVEC_CHECK_INTERRUPTS()	CHECK_FOR_INTERRUPTS()
#else
#define LIONVEC_CHECK_INTERRUPTS()	((void) 0)
#endif

/* how far apart the halves of a split centroid start, as FAISS has it */
#define LIONVEC_SPLIT_EPS		(1.0f / 1024.0f)

/* lionvec_subsample()'s mark of a slot drawn: no slot below INT_MAX has it */
#define LIONVEC_SLOT_MARK		((uint32) 1 << 31)

void
lionvec_subsample(int n, int m, uint64 seed, uint32 *slot)
{
	pg_prng_state st;
	int			c = 0;
	int			i;

	Assert(n >= 0 && m >= 0 && m <= n);
	pg_prng_seed(&st, seed);
	for (i = 0; i < n; i++)
		slot[i] = (uint32) i;
	/* each of the first m places a slot drawn from those not yet drawn */
	for (i = 0; i < m; i++)
	{
		uint32		r = (uint32) i +
			(uint32) pg_prng_uint64_range(&st, 0, (uint64) (n - 1 - i));
		uint32		t = slot[i];

		LIONVEC_CHECK_INTERRUPTS();
		slot[i] = slot[r];
		slot[r] = t;
	}

	/*
	 * ... and those into ascending order, without another array: each slot
	 * drawn is marked by the top bit of the entry at its own place, which no
	 * slot's number has, n being an int; then the marked places are gathered
	 * down, each entry read before anything is written over it.
	 */
	for (i = 0; i < m; i++)
	{
		uint32		v = slot[i] & ~LIONVEC_SLOT_MARK;

		slot[v] |= LIONVEC_SLOT_MARK;
	}
	for (i = 0; i < n; i++)
	{
		LIONVEC_CHECK_INTERRUPTS();
		if ((slot[i] & LIONVEC_SLOT_MARK) != 0)
			slot[c++] = (uint32) i;
	}
	Assert(c == m);
}

void
lionvec_kmeans_start(const LionVecSample *s, int k, bool unit, uint64 seed,
					 float *cent, int *assign)
{
	int			dimsp = s->dimsp;
	int			n = s->n;
	pg_prng_state st;
	int			i;
	int			j;

	Assert(dimsp >= 64 && dimsp % 64 == 0);
	Assert(k >= 1 && k <= n);

	/* k distinct rows to start from: a partial Fisher-Yates shuffle */
	pg_prng_seed(&st, seed);
	for (i = 0; i < n; i++)
		assign[i] = i;
	for (j = 0; j < k; j++)
	{
		int			r = j + (int) pg_prng_uint64_range(&st, 0, (uint64) (n - 1 - j));
		int			t = assign[j];

		assign[j] = assign[r];
		assign[r] = t;
		lionvec_sample_row(s, assign[j], cent + (Size) j * dimsp);
		if (unit)
			lionvec_unit(cent + (Size) j * dimsp, dimsp);
	}
	for (i = 0; i < n; i++)
		assign[i] = -1;
}

void
lionvec_kmeans_codes(int k, int dimsp, const float *mean, const float *cent,
					 LionVecKmeansWork *w)
{
	int			nw = dimsp / 64;
	int			j;

	for (j = 0; j < k; j++)
	{
		LionVecEncoded e;

		LIONVEC_CHECK_INTERRUPTS();
		memcpy(w->x, cent + (Size) j * dimsp, sizeof(float) * dimsp);
		e.bits = w->bits + (Size) j * nw;
		e.codes = w->codes;
		lionvec_encode(mean, dimsp, w->x, &e);
		w->popcount[j] = e.popcount;
		w->norm[j] = e.norm;
		w->alpha1[j] = e.alpha1;
	}
}

int
lionvec_kmeans_assign(const LionVecSample *s, int from, int to, int k,
					  const float *mean, const float *cent, int *assign,
					  LionVecKmeansWork *w, const LionVecRegions *r)
{
	int			dimsp = s->dimsp;
	bool		routed = r != NULL && r->n > 0;
	LionVecCentroids cs;
	LionVecQuery q;
	int			moved = 0;
	int			i;

	Assert(from >= 0 && from <= to && to <= s->n);
	Assert(!routed || (r->k == k && r->dimsp == dimsp));
	cs.k = k;
	cs.dimsp = dimsp;
	cs.bits = w->bits;
	cs.popcount = w->popcount;
	cs.norm = w->norm;
	cs.alpha1 = w->alpha1;
	q.dimsp = dimsp;
	q.qc = w->qc;
	q.planes = w->planes;

	for (i = from; i < to; i++)
	{
		int			nc;
		int			best = -1;
		double		bestd = INFINITY;
		int			c;

		LIONVEC_CHECK_INTERRUPTS();
		lionvec_sample_row(s, i, w->y);
		lionvec_query_init(&q, LIONVEC_L2, mean, w->y, LIONVEC_ASSIGN_EPS);
		nc = routed ?
			lionvec_route(r, &cs, &q, 1, &w->route, w->lo, w->hi, w->cand) :
			lionvec_candidates(&cs, &q, w->lo, w->hi, w->cand);
		for (c = 0; c < nc; c++)
		{
			double		d = lionvec_l2sq(w->y, cent + (Size) w->cand[c] * dimsp,
										 dimsp);

			/*
			 * in the candidates' order - the centroids', or their regions'
			 * and then theirs - so that ties go the same way every time
			 */
			if (best < 0 || d < bestd)
			{
				best = w->cand[c];
				bestd = d;
			}
		}
		Assert(best >= 0);
		if (assign[i] != best)
			moved++;
		assign[i] = best;
	}
	return moved;
}

void
lionvec_kmeans_update(const LionVecSample *s, int k, bool unit,
					  const int *assign, float *cent, LionVecKmeansWork *w)
{
	int			dimsp = s->dimsp;
	int			n = s->n;
	int			i;
	int			j;

	memset(w->count, 0, sizeof(int) * k);
	for (i = 0; i < n; i++)
	{
		Assert(assign[i] >= 0 && assign[i] < k);
		w->count[assign[i]]++;
	}

	/* the means, summed where the centroids were: they are not needed now */
	for (j = 0; j < k; j++)
	{
		if (w->count[j] > 0)
			memset(cent + (Size) j * dimsp, 0, sizeof(float) * dimsp);
	}
	for (i = 0; i < n; i++)
	{
		float	   *sum = cent + (Size) assign[i] * dimsp;
		int			d;

		LIONVEC_CHECK_INTERRUPTS();
		lionvec_sample_row(s, i, w->y);
		for (d = 0; d < dimsp; d++)
			sum[d] += w->y[d];
	}
	for (j = 0; j < k; j++)
	{
		if (w->count[j] > 0)
		{
			float		inv = 1.0f / (float) w->count[j];
			float	   *c = cent + (Size) j * dimsp;
			int			d;

			for (d = 0; d < dimsp; d++)
				c[d] *= inv;
		}
	}
	for (j = 0; j < k; j++)
	{
		int			l = 0;
		int			m;
		float	   *cj = cent + (Size) j * dimsp;
		float	   *cl;
		int			d;

		if (w->count[j] > 0)
			continue;
		LIONVEC_CHECK_INTERRUPTS();
		/* the largest has two rows or more, as k <= n */
		for (m = 1; m < k; m++)
			if (w->count[m] > w->count[l])
				l = m;
		Assert(w->count[l] >= 2);
		cl = cent + (Size) l * dimsp;
		for (d = 0; d < dimsp; d++)
		{
			float		v = cl[d];

			cj[d] = v * (d % 2 == 0 ? 1.0f + LIONVEC_SPLIT_EPS : 1.0f - LIONVEC_SPLIT_EPS);
			cl[d] = v * (d % 2 == 0 ? 1.0f - LIONVEC_SPLIT_EPS : 1.0f + LIONVEC_SPLIT_EPS);
		}
		w->count[j] = w->count[l] / 2;
		w->count[l] -= w->count[j];
	}
	if (unit)
	{
		for (j = 0; j < k; j++)
			lionvec_unit(cent + (Size) j * dimsp, dimsp);
	}
}

int
lionvec_kmeans(const LionVecSample *s, int k, int iters, bool unit,
			   uint64 seed, const float *mean, float *cent,
			   LionVecKmeansWork *w, LionVecRegions *r, LionVecRegionMaker *mk)
{
	bool		routed = r != NULL && r->n > 0;
	int			made = 0;

	Assert(!routed || (mk != NULL && r->k == k && r->dimsp == s->dimsp));
	lionvec_kmeans_start(s, k, unit, seed, cent, w->assign);
	while (made < iters)
	{
		made++;
		/* the centroids' codes, which the candidates are found by */
		lionvec_kmeans_codes(k, s->dimsp, mean, cent, w);
		/* ... and their regions, which the last iteration moved them between */
		if (routed)
			lionvec_regions_make(r, cent, unit, LIONVEC_REGION_SEED(seed), mean, mk);
		/* each row to its nearest; none moved: they are already its means */
		if (lionvec_kmeans_assign(s, 0, s->n, k, mean, cent, w->assign, w,
								  routed ? r : NULL) == 0)
			break;
		lionvec_kmeans_update(s, k, unit, w->assign, cent, w);
	}
	return made;
}

/* ----------------------------------------------------------------
 * Regions: the centroids gathered, and a row routed through them
 * ----------------------------------------------------------------
 */

double
lionvec_region_key(const LionVecQuery *q, const uint64 *bits, uint16 popcount,
				   float norm, float alpha1)
{
	double		est;
	double		err;
	double		key;

	lionvec_estimate1(q, bits, popcount, norm, alpha1, &est, &err);
	key = (double) norm * norm - 2.0 * est;
	return isnan(key) ? INFINITY : key;
}

int
lionvec_regions_pick(const double *key, const int *first, int n, int beam,
					 int need, int *order, int *pick)
{
	int			want = Min(Max(beam, 1), n);
	int			got = 0;
	int			t;

	if (n <= 0)
		return 0;
	for (;;)
	{
		int			r = lionvec_rank(key, n, want, order);
		int			members = 0;

		got = 0;
		for (t = 0; t < r && (got < beam || members < need); t++)
		{
			int			g = order[t];
			int			count = first[g + 1] - first[g];

			if (count <= 0)
				continue;
			pick[got++] = g;
			members += count;
		}
		if ((got >= beam && members >= need) || r == n)
			break;
		/* the nearest had too few members, or none: rank further */
		want = Min(n, 2 * want);
	}

	/* ascending, so that their members come in the centroids' order */
	for (t = 1; t < got; t++)
	{
		int			g = pick[t];
		int			i = t;

		for (; i > 0 && pick[i - 1] > g; i--)
			pick[i] = pick[i - 1];
		pick[i] = g;
	}
	return got;
}

int
lionvec_route(const LionVecRegions *r, const LionVecCentroids *c,
			  const LionVecQuery *q, int m, LionVecRoute *rt, double *lo,
			  double *hi, int *cand)
{
	int			nw = r->dimsp / 64;
	int			np;
	int			n = 0;
	int			g;
	int			t;

	Assert(r->n > 0 && r->k == c->k && r->dimsp == c->dimsp);
	for (g = 0; g < r->n; g++)
		rt->key[g] = r->first[g + 1] > r->first[g] ?
			lionvec_region_key(q, r->bits + (Size) g * nw, r->popcount[g],
							   r->norm[g], r->alpha1[g]) : INFINITY;
	np = lionvec_regions_pick(rt->key, r->first, r->n,
							  lionvec_region_beam(r->n, m), m, rt->order,
							  rt->pick);
	for (t = 0; t < np; t++)
	{
		int			i;

		g = rt->pick[t];
		for (i = r->first[g]; i < r->first[g + 1]; i++)
			rt->idx[n++] = r->member[i];
	}
	Assert(n <= c->k);
	return lionvec_candidates_among(c, rt->idx, n, q, m, lo, hi, cand);
}

void
lionvec_regions_make(LionVecRegions *r, const float *cent, bool unit,
					 uint64 seed, const float *mean, LionVecRegionMaker *mk)
{
	int			dimsp = r->dimsp;
	int			nw = dimsp / 64;
	int			k = r->k;
	int			n = r->n;
	LionVecSample s;
	int			g;
	int			j;

	Assert(n >= 1 && n <= k);
	/* the centroids as a sample is: 8-bit codes, rotated and not centred */
	for (j = 0; j < k; j++)
	{
		LionVecEncoded e;

		LIONVEC_CHECK_INTERRUPTS();
		memcpy(mk->w.x, cent + (Size) j * dimsp, sizeof(float) * dimsp);
		e.bits = mk->w.bits;	/* scratch, which k-means writes over */
		e.codes = mk->codes + (Size) j * dimsp;
		lionvec_encode(NULL, dimsp, mk->w.x, &e);
		mk->scale[j] = e.rnorm8 > 0.0f ? e.norm / e.rnorm8 : 0.0f;
	}
	s.n = k;
	s.dimsp = dimsp;
	s.codes = mk->codes;
	s.scale = mk->scale;
	(void) lionvec_kmeans(&s, n, LIONVEC_REGION_ITERS, unit, seed, mean,
						  mk->cent, &mk->w, NULL, NULL);

	/*
	 * each centroid into the region whose centroid is nearest it now, found
	 * as k-means finds a row's: its last update moved the regions'
	 * centroids after its last assignment
	 */
	lionvec_kmeans_codes(n, dimsp, mean, mk->cent, &mk->w);
	(void) lionvec_kmeans_assign(&s, 0, k, n, mean, mk->cent, mk->w.assign,
								 &mk->w, NULL);

	/* the members, region by region, each region's in order */
	for (g = 0; g <= n; g++)
		r->first[g] = 0;
	for (j = 0; j < k; j++)
	{
		Assert(mk->w.assign[j] >= 0 && mk->w.assign[j] < n);
		r->first[mk->w.assign[j] + 1]++;
	}
	for (g = 0; g < n; g++)
		r->first[g + 1] += r->first[g];
	for (g = 0; g < n; g++)
		mk->w.count[g] = r->first[g];
	for (j = 0; j < k; j++)
		r->member[mk->w.count[mk->w.assign[j]]++] = j;

	/* the regions' centroids coded centred, as lionvec_kmeans_codes() made them */
	memcpy(r->bits, mk->w.bits, sizeof(uint64) * (Size) n * nw);
	memcpy(r->popcount, mk->w.popcount, sizeof(uint16) * n);
	memcpy(r->norm, mk->w.norm, sizeof(float) * n);
	memcpy(r->alpha1, mk->w.alpha1, sizeof(float) * n);
}

/* ----------------------------------------------------------------
 * Estimates of <x, qc> and their errors
 * ----------------------------------------------------------------
 */

/*
 * From plane 1: <sign(x), qc> is 2 sum(b_i qc_i) - sum(qc_i) over the bits
 * b, and sum(b_i qc_i) is qlo popcount(b) + qdelta sum_j 2^j popcount(b &
 * plane j) of the quantized query.
 */
void
lionvec_estimate1(const LionVecQuery *q, const uint64 *bits, uint16 popcount,
				  float norm, float alpha1, double *est, double *err)
{
	uint32		dot = lionvec_plane_dot(bits, q->planes, q->dimsp / 64);
	double		sbq = q->qlo * popcount + q->qdelta * dot;
	double		ip = (2.0 * sbq - q->qsum) / sqrt((double) q->dimsp);
	double		a = Max((double) alpha1, 1e-6);
	double		miss = Max(1.0 - a * a, 0.0);

	*est = norm * ip / a;
	*err = q->eps * norm / a * sqrt(q->var1 * miss + q->varq);
}

/* From the 8-bit codes, against qc itself. */
void
lionvec_estimate8(const LionVecQuery *q, const uint8 *codes, float norm,
				  float alpha8, float rnorm8, double *est, double *err)
{
	double		s = lionvec_sum8(codes, q->qc, q->dimsp);
	double		a = Max((double) alpha8, 1e-6);
	double		miss = Max(1.0 - a * a, 0.0);

	*est = rnorm8 > 0.0f ? norm * s / (rnorm8 * a) : 0.0;
	*err = LIONVEC_EPS8(q->eps) * norm / a * sqrt(q->var1 * miss);
}
