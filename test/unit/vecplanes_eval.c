/*-------------------------------------------------------------------------
 * vecplanes_eval.c
 *	  An evaluation, not a test (DESIGN.md §44.14): would lionvec's 8-bit
 *	  codes, kept as seven more bit-planes - planes 2 to 8 of each code, most
 *	  significant first, plane 1 its top bit - let a refine stop before it
 *	  has read them all?  A refine would read plane 2, bound the row by the
 *	  reconstruction from its codes' top two bits, and stop once that bound
 *	  put the row past the k-th; else read plane 3; and so on to plane 8,
 *	  which is the codes as a refine reads them today.
 *
 *	  With lion_vec_math.c's encoder and estimates, linked and compiled as
 *	  vec_test links them, it measures:
 *
 *	  1. the prefixes: alpha_m of each row's reconstruction from its codes'
 *		 top m bits, against a Gaussian coordinate's, and how often the bound
 *		 derived from it fails at 1, 2 and 3 standard deviations - as often
 *		 as the Gaussian says, if the bound is right;
 *	  2. the planes a refine needs: on vec_test's kinds of data, or on the
 *		 vectors of a file (--file: DBpedia's, for §44.14), for every row -
 *		 of every partition, and of those a scan by the margin alone probes -
 *		 and every centroid, less the margin, whose plane-1 lower bound is
 *		 below the k-th row's key (the rows and centroids a scan refines),
 *		 the first plane at which the prefix's lower bound passes that key;
 *		 the prefix sums in double from the query's floats, or from a query
 *		 quantized to QB_BITS bits by popcounts;
 *	  3. what a refine's sums cost - lionvec_sum8(), planes 2 to m both
 *		 ways, and for comparison the bytes against a query rounded to
 *		 16-bit integers - under each implementation the CPU runs: warm, from
 *		 a pool of rows past the L2, and with their lines flushed first
 *		 (--costs: this part alone).
 *
 *	  The kernels here (the masked sums, the integer dot) are the
 *	  evaluation's, each checked against its portable form; none is
 *	  lionvec's.  make vecplanes-eval PG_CONFIG=... builds it at -O2 and
 *	  runs it; --quick runs smaller data.  It prints what it measured; it
 *	  checks only that the prefix of all eight planes is the codes' own
 *	  estimate, that its kernels agree with their portable forms, and that
 *	  the prefixes' bounds are calibrated, and exits 1 if not.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lion_vec_math.h"

#ifdef LION_HAVE_X86_SIMD
#include <immintrin.h>
#endif

#define K_NEAREST		10		/* LIMIT 10 */
#define EPS				4.0		/* pg_lion.vector_epsilon's default */
#define MARGIN			3.0		/* pg_lion.vector_margin's default */
#define QB_BITS			8		/* the quantized query's bits a dimension */
#define NPLANES			8

static long nfail = 0;

#define CHECK(ok, msg) \
	do { if (!(ok)) { nfail++; printf("FAIL vecplanes_eval.c:%d: %s\n", __LINE__, (msg)); } } while (0)

/* vec_test's generator, so that its kinds of data come out as they do there */
static uint64 rng_state = 0x9E3779B97F4A7C15ULL;

static uint64
rnd64(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static double
rnd_unit(void)
{
	return ((double) (rnd64() >> 11) + 0.5) / 9007199254740992.0;
}

static double
rnd_gauss(void)
{
	return sqrt(-2.0 * log(rnd_unit())) * cos(2.0 * M_PI * rnd_unit());
}

static void *
xmalloc(Size n)
{
	void	   *p = malloc(n > 0 ? n : 1);

	if (p == NULL)
	{
		printf("out of memory\n");
		exit(2);
	}
	return p;
}

static float *
falloc(Size n)
{
	return (float *) xmalloc(sizeof(float) * n);
}

/* on cache lines of their own, so that a row's lines are its own */
static void *
xmalloc_lines(Size n)
{
	void	   *p = aligned_alloc(64, TYPEALIGN(64, Max(n, 1)));

	if (p == NULL)
	{
		printf("out of memory\n");
		exit(2);
	}
	return p;
}

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

/* ---------- the prefixes ---------- */

/*
 * The planes of a code c: plane p, 1 to 8, is bit 8 - p, so plane 1 is the
 * top bit, which is plane 1 of the CODE item but where the coordinate is a
 * zero that rounded up (counted below), and the top m planes are the code
 * truncated to its top m bits.  Their reconstruction is the middle of the
 * 2^(8 - m) steps they leave:
 *
 *		r_m(c) = -4 + (c_m + 2^(7 - m)) / 32,	c_m = c with its 8 - m low bits 0
 *			   = (-4 + 2^(2 - m)) + sum over p <= m of 2^(3 - p) b_p
 *
 * - r_8 is lionvec_recon8[c], and r_1 is 2 sign(c) - linear in the planes'
 * bits b_p, so that <r_m, v> = (-4 + 2^(2 - m)) sum(v) + sum over p <= m of
 * 2^(3 - p) S_p, S_p the sum of v over the dimensions plane p has set: one
 * more S_p a plane.
 */
static double
prefix_c0(int m)
{
	return -4.0 + ldexp(1.0, 2 - m);
}

static double
prefix_w(int p)
{
	return ldexp(1.0, 3 - p);
}

static double
recon_m(uint8 c, int m)
{
	int			cm = c & ~((1 << (8 - m)) - 1);

	return -4.0 + (cm + ldexp(1.0, 7 - m)) / 32.0;
}

/*
 * The sums.  F: S_p in double from the query's floats, the i-th term into
 * lane i % 16 and the lanes added up as lion_vec_math.c's kernels add them,
 * a masked sum of floats a plane - portable, AVX2 (and with a mask, compare
 * and AND) and AVX-512VL (a masked add, 256 bits), each the same to the bit.
 * Q: S_p of the query quantized to QB_BITS bits, qlo popcount(P_p) + qdelta
 * sum_j 2^j popcount(P_p AND Q_j), the weighted sum lion_planes.c computes
 * of QB_BITS interleaved planes.
 */
static inline double
lanes_sum(const double *p)
{
	double		u[4];
	int			l;

	for (l = 0; l < 4; l++)
		u[l] = (p[l] + p[l + 8]) + (p[l + 4] + p[l + 12]);
	return (u[0] + u[2]) + (u[1] + u[3]);
}

static double
psum_portable(const uint64 *plane, const double *qd, int nwords)
{
	double		p[16] = {0};
	int			w;
	int			b;
	int			l;

	for (w = 0; w < nwords; w++)
	{
		uint64		bits = plane[w];
		const double *q = qd + w * 64;

		for (b = 0; b < 64; b += 16)
			for (l = 0; l < 16; l++)
				p[l] += ((bits >> (b + l)) & 1) ? q[b + l] : 0.0;
	}
	return lanes_sum(p);
}

/* the sum of v over every dimension, in the same lanes */
static double
vsum_lanes(const double *qd, int D)
{
	double		p[16] = {0};
	int			i;

	for (i = 0; i < D; i++)
		p[i % 16] += qd[i];
	return lanes_sum(p);
}

#ifdef LION_HAVE_X86_SIMD

static inline LION_TARGET("avx2") double
fold_avx2(__m256d p0, __m256d p4, __m256d p8, __m256d p12)
{
	__m256d		u = _mm256_add_pd(_mm256_add_pd(p0, p8), _mm256_add_pd(p4, p12));
	__m128d		v = _mm_add_pd(_mm256_castpd256_pd128(u), _mm256_extractf128_pd(u, 1));

	return _mm_cvtsd_f64(_mm_add_sd(v, _mm_unpackhi_pd(v, v)));
}

static LION_TARGET("avx2") double
psum_avx2(const uint64 *plane, const double *qd, int nwords)
{
	__m256d		p[4];
	__m256i		sel[16];
	int			w;
	int			g;
	int			j;

	for (j = 0; j < 4; j++)
		p[j] = _mm256_setzero_pd();
	for (g = 0; g < 16; g++)
		sel[g] = _mm256_set_epi64x((int64) 1 << (4 * g + 3), (int64) 1 << (4 * g + 2),
								   (int64) 1 << (4 * g + 1), (int64) 1 << (4 * g));
	for (w = 0; w < nwords; w++)
	{
		__m256i		bw = _mm256_set1_epi64x((int64) plane[w]);
		const double *q = qd + w * 64;

		for (g = 0; g < 16; g++)
		{
			__m256i		m = _mm256_cmpeq_epi64(_mm256_and_si256(bw, sel[g]), sel[g]);
			__m256d		t = _mm256_and_pd(_mm256_loadu_pd(q + 4 * g), _mm256_castsi256_pd(m));

			p[g & 3] = _mm256_add_pd(p[g & 3], t);
		}
	}
	return fold_avx2(p[0], p[1], p[2], p[3]);
}

#ifdef LION_HAVE_X86_AVX512
static LION_TARGET("avx2,avx512f,avx512vl") double
psum_avx512(const uint64 *plane, const double *qd, int nwords)
{
	__m256d		p0 = _mm256_setzero_pd();
	__m256d		p4 = _mm256_setzero_pd();
	__m256d		p8 = _mm256_setzero_pd();
	__m256d		p12 = _mm256_setzero_pd();
	int			w;
	int			b;

	for (w = 0; w < nwords; w++)
	{
		uint64		bits = plane[w];
		const double *q = qd + w * 64;

		for (b = 0; b < 64; b += 16)
		{
			p0 = _mm256_mask_add_pd(p0, (__mmask8) ((bits >> b) & 0xF), p0, _mm256_loadu_pd(q + b));
			p4 = _mm256_mask_add_pd(p4, (__mmask8) ((bits >> (b + 4)) & 0xF), p4, _mm256_loadu_pd(q + b + 4));
			p8 = _mm256_mask_add_pd(p8, (__mmask8) ((bits >> (b + 8)) & 0xF), p8, _mm256_loadu_pd(q + b + 8));
			p12 = _mm256_mask_add_pd(p12, (__mmask8) ((bits >> (b + 12)) & 0xF), p12, _mm256_loadu_pd(q + b + 12));
		}
	}
	return fold_avx2(p0, p4, p8, p12);
}
#endif
#endif							/* LION_HAVE_X86_SIMD */

/*
 * For comparison, a kernel and not a format: the codes as the bytes they
 * are, against the query rounded to 16-bit integers, sum of c_i w_i exact in
 * integers (whatever the implementation), from which <r_8, q> follows as
 * an affine function of it, sum(c_i) and sum(w_i).  AVX2: VPMADDWD, the
 * codes widened to 16 bits, sixteen a step into eight 32-bit lanes (at
 * most 2 * 255 * 32767 a step, so 64 steps before they are widened).
 */
static int64
idot16_portable(const uint8 *codes, const int16 *w, int D)
{
	int64		s = 0;
	int			i;

	for (i = 0; i < D; i++)
		s += (int64) codes[i] * w[i];
	return s;
}

#ifdef LION_HAVE_X86_SIMD
static LION_TARGET("avx2") int64
idot16_avx2(const uint8 *codes, const int16 *w, int D)
{
	__m256i		acc64 = _mm256_setzero_si256();
	int			i = 0;

	while (i < D)
	{
		__m256i		acc = _mm256_setzero_si256();
		int			end = Min(D, i + 64 * 16);

		for (; i < end; i += 16)
			acc = _mm256_add_epi32(acc,
								   _mm256_madd_epi16(_mm256_cvtepu8_epi16(_mm_loadu_si128((const __m128i *) (codes + i))),
													 _mm256_loadu_si256((const __m256i *) (w + i))));
		acc64 = _mm256_add_epi64(acc64, _mm256_cvtepi32_epi64(_mm256_castsi256_si128(acc)));
		acc64 = _mm256_add_epi64(acc64, _mm256_cvtepi32_epi64(_mm256_extracti128_si256(acc, 1)));
	}
	{
		int64		l[4];

		_mm256_storeu_si256((__m256i *) l, acc64);
		return (l[0] + l[1]) + (l[2] + l[3]);
	}
}
#endif

static int64
idot16(const uint8 *codes, const int16 *w, int D)
{
#ifdef LION_HAVE_X86_SIMD
	LionSimdImpl impl = lion_simd_current();

	if (impl == LION_SIMD_AVX2 || impl == LION_SIMD_AVX512)
		return idot16_avx2(codes, w, D);
#endif
	return idot16_portable(codes, w, D);
}

/* the masked sum the implementation in use would run */
static double
psum(const uint64 *plane, const double *qd, int nwords)
{
#ifdef LION_HAVE_X86_SIMD
	LionSimdImpl impl = lion_simd_current();

#ifdef LION_HAVE_X86_AVX512
	if (impl == LION_SIMD_AVX512)
		return psum_avx512(plane, qd, nwords);
#endif
	if (impl == LION_SIMD_AVX2 || impl == LION_SIMD_AVX512)
		return psum_avx2(plane, qd, nwords);
#endif
	return psum_portable(plane, qd, nwords);
}

/* ---------- rows, queries ---------- */

typedef struct Rows
{
	int			n;
	int			D;
	int			nw;
	float	   *xc;				/* [n][D]: centred, rotated */
	float	   *norm;
	float	   *xm;
	float	   *alpha1;
	uint16	   *pop;
	uint64	   *bits;			/* [n][nw]: plane 1, the signs */
	uint8	   *codes;			/* [n][D] */
	uint64	   *planes;			/* [n][8][nw]: plane p at (p - 1) nw */
	float	   *alpha;			/* [n][9]: alpha_m, m = 1 .. 8 */
	float	   *rnorm;			/* [n][9]: |r_m| */
	long		topbit_off;		/* code's top bit not the sign bit */
} Rows;

static void
rows_alloc(Rows *r, int n, int D)
{
	memset(r, 0, sizeof(*r));
	r->n = n;
	r->D = D;
	r->nw = D / 64;
	r->xc = falloc((Size) n * D);
	r->norm = falloc(n);
	r->xm = falloc(n);
	r->alpha1 = falloc(n);
	r->pop = (uint16 *) xmalloc(sizeof(uint16) * n);
	r->bits = (uint64 *) xmalloc(sizeof(uint64) * n * r->nw);
	r->codes = (uint8 *) xmalloc_lines((Size) n * D);
	r->planes = (uint64 *) xmalloc_lines(sizeof(uint64) * n * NPLANES * r->nw);
	r->alpha = falloc((Size) n * 9);
	r->rnorm = falloc((Size) n * 9);
}

static void
rows_free(Rows *r)
{
	free(r->xc);
	free(r->norm);
	free(r->xm);
	free(r->alpha1);
	free(r->pop);
	free(r->bits);
	free(r->codes);
	free(r->planes);
	free(r->alpha);
	free(r->rnorm);
}

/* Row i from the rotated x[D] (centred here, as lionvec_encode() does). */
static void
rows_encode(Rows *r, int i, const float *mean, const float *x)
{
	int			D = r->D;
	int			nw = r->nw;
	float	   *xc = r->xc + (Size) i * D;
	uint8	   *codes = r->codes + (Size) i * D;
	uint64	   *planes = r->planes + (Size) i * NPLANES * nw;
	LionVecEncoded e;
	int			m;
	int			d;
	double		xx = 0.0;

	memcpy(xc, x, sizeof(float) * D);
	e.bits = r->bits + (Size) i * nw;
	e.codes = codes;
	lionvec_encode(mean, D, xc, &e);	/* centres xc */
	r->norm[i] = e.norm;
	r->xm[i] = e.xm;
	r->alpha1[i] = e.alpha1;
	r->pop[i] = e.popcount;

	memset(planes, 0, sizeof(uint64) * NPLANES * nw);
	for (d = 0; d < D; d++)
	{
		int			p;

		for (p = 1; p <= NPLANES; p++)
			if ((codes[d] >> (8 - p)) & 1)
				planes[(p - 1) * nw + d / 64] |= UINT64CONST(1) << (d % 64);
		xx += (double) xc[d] * xc[d];
	}
	for (d = 0; d < nw; d++)
		r->topbit_off += pg_popcount64(planes[d] ^ e.bits[d]);

	for (m = 1; m <= NPLANES; m++)
	{
		double		rr = 0.0;
		double		rx = 0.0;

		for (d = 0; d < D; d++)
		{
			double		rm = recon_m(codes[d], m);

			rr += rm * rm;
			rx += rm * xc[d];
		}
		r->rnorm[(Size) i * 9 + m] = (float) sqrt(rr);
		r->alpha[(Size) i * 9 + m] = xx > 0.0 ? (float) (rx / sqrt(rr * xx)) : 1.0f;
	}
}

typedef struct Query
{
	LionVecQuery q;				/* lionvec's, for plane 1 and the codes */
	double	   *qd;				/* [D]: qc in double */
	double		sumqc;			/* its sum, in lanes */
	uint64	   *qb;				/* [nw][QB_BITS]: interleaved planes */
	double		qblo;
	double		qbdelta;
	double		qbsum;			/* sum of the quantized qc */
	double		varqb;			/* the quantization's variance a dimension */
	int16	   *qw;				/* [D]: qc rounded to 16-bit integers */
} Query;

static void
query_alloc(Query *Q, int D)
{
	Q->q.dimsp = D;
	Q->q.qc = falloc(D);
	Q->q.planes = (uint64 *) xmalloc(sizeof(uint64) * (D / 64) * LIONVEC_QBITS);
	Q->qd = (double *) xmalloc(sizeof(double) * D);
	Q->qb = (uint64 *) xmalloc(sizeof(uint64) * (D / 64) * QB_BITS);
	Q->qw = (int16 *) xmalloc(sizeof(int16) * D);
}

static void
query_free(Query *Q)
{
	free(Q->q.qc);
	free(Q->q.planes);
	free(Q->qd);
	free(Q->qb);
	free(Q->qw);
}

static void
query_init(Query *Q, int metric, const float *mean, const float *x, double eps)
{
	int			D = Q->q.dimsp;
	int			levels = (1 << QB_BITS) - 1;
	uint64	   *u = (uint64 *) xmalloc(sizeof(uint64) * D);
	double		mn = INFINITY;
	double		mx = -INFINITY;
	double		e2 = 0.0;
	double		s = 0.0;
	int			i;

	lionvec_query_init(&Q->q, metric, mean, x, eps);
	for (i = 0; i < D; i++)
	{
		Q->qd[i] = Q->q.qc[i];
		mn = Min(mn, Q->qd[i]);
		mx = Max(mx, Q->qd[i]);
	}
	Q->sumqc = vsum_lanes(Q->qd, D);
	Q->qblo = mn;
	Q->qbdelta = mx > mn ? (mx - mn) / levels : 1.0;
	for (i = 0; i < D; i++)
	{
		double		t = (Q->qd[i] - mn) / Q->qbdelta + 0.5;
		int			v = t >= levels ? levels : (t > 0.0 ? (int) t : 0);
		double		qq = mn + Q->qbdelta * v;

		u[i] = (uint64) v;
		s += qq;
		e2 += (Q->qd[i] - qq) * (Q->qd[i] - qq);
	}
	lion_planes_scatter_interleaved(u, D, QB_BITS, Q->qb);
	for (i = 0; i < D; i++)
		Q->qw[i] = (int16) lrint((Q->qd[i] - (mx + mn) / 2) / (mx > mn ? (mx - mn) / 65534 : 1.0));
	Q->qbsum = s;
	Q->varqb = e2 / D;
	free(u);
}

/*
 * The prefix estimates of row i for m = 1 .. 8, with their errors at
 * eps = 1 (the caller scales): from the floats (F) and from the quantized
 * query (Q).  est_m = |x| <r_m, qc> / (|r_m| alpha_m), and its error
 * |x| / alpha_m sqrt(var1 (1 - alpha_m^2) + varq), varq the quantization's
 * variance (Q's, none for F) - plane 1's form and the codes' (§44.3).
 */
typedef struct Prefix
{
	double		estf[NPLANES + 1];
	double		errf[NPLANES + 1];
	double		estq[NPLANES + 1];
	double		errq[NPLANES + 1];
} Prefix;

static void
prefix_estimates(const Rows *r, int i, const Query *Q, Prefix *P)
{
	const uint64 *planes = r->planes + (Size) i * NPLANES * r->nw;
	double		dotf = 0.0;
	double		dotq = 0.0;
	double		norm = r->norm[i];
	int			m;

	for (m = 1; m <= NPLANES; m++)
	{
		const uint64 *pl = planes + (m - 1) * r->nw;
		double		sf = psum(pl, Q->qd, r->nw);
		uint32		pop = 0;
		uint64		dot = lion_planes_dot_interleaved(pl, Q->qb, (uint32) r->nw, QB_BITS);
		double		sq;
		double		a = Max((double) r->alpha[(Size) i * 9 + m], 1e-6);
		double		rn = r->rnorm[(Size) i * 9 + m];
		double		miss = Max(1.0 - a * a, 0.0);
		int			w;

		for (w = 0; w < r->nw; w++)
			pop += pg_popcount64(pl[w]);
		sq = Q->qblo * pop + Q->qbdelta * (double) dot;
		dotf += prefix_w(m) * sf;
		dotq += prefix_w(m) * sq;

		P->estf[m] = rn > 0.0 ? norm * (prefix_c0(m) * Q->sumqc + dotf) / (rn * a) : 0.0;
		P->errf[m] = norm / a * sqrt(Q->q.var1 * miss);
		P->estq[m] = rn > 0.0 ? norm * (prefix_c0(m) * Q->qbsum + dotq) / (rn * a) : 0.0;
		P->errq[m] = norm / a * sqrt(Q->q.var1 * miss + Q->varqb);
	}
}

/* ---------- data: vec_test's kinds ---------- */

enum
{
	DATA_CLUSTERS,				/* vec_test's make_clusters(): 12 centres */
	DATA_GAUSS,					/* isotropic: random unit vectors, for cosine */
	DATA_TIGHT,					/* many tight clusters, as test_candidates()'s */
	NDATA
};

static const char *const data_names[NDATA] = {"clusters", "gaussian", "tight"};

static void
make_data(int kind, float *rows, int n, float *queries, int nq, int dims)
{
	int			k = kind == DATA_CLUSTERS ? 12 : (kind == DATA_TIGHT ? 200 : 0);
	float	   *centres = falloc((Size) Max(k, 1) * dims);
	double		rs = kind == DATA_CLUSTERS ? 0.6 : 0.4;
	double		qs = kind == DATA_CLUSTERS ? 0.7 : 0.4;
	int			i;
	int			d;

	for (i = 0; i < k * dims; i++)
		centres[i] = kind == DATA_CLUSTERS ?
			(float) (rnd_gauss() / sqrt((double) dims) + 0.02) :
			(float) (rnd_gauss() / sqrt((double) dims));
	for (i = 0; i < n + nq; i++)
	{
		float	   *v = i < n ? rows + (Size) i * dims : queries + (Size) (i - n) * dims;
		int			c = k > 0 ? (int) (rnd64() % k) : 0;
		double		s = i < n ? rs : qs;

		for (d = 0; d < dims; d++)
			v[d] = k > 0 ?
				(float) (centres[(Size) c * dims + d] + s * rnd_gauss() / sqrt((double) dims)) :
				(float) rnd_gauss();
	}
	free(centres);
}

/* ---------- 1. the prefixes ---------- */

static void
eval_prefixes(const Rows *r, const float *mean, const float *qx, int nq, int metric)
{
	Query		Q;
	Prefix		P;
	double		asum[NPLANES + 1] = {0};
	double		a1sum = 0.0;
	long		outf[NPLANES + 1][3] = {{0}};
	long		outq[NPLANES + 1][3] = {{0}};
	long		n = 0;
	double		maxdiff = 0.0;
	int			i;
	int			j;
	int			m;

	for (i = 0; i < r->n; i++)
	{
		a1sum += r->alpha1[i];
		for (m = 1; m <= NPLANES; m++)
			asum[m] += r->alpha[(Size) i * 9 + m];
	}
	printf("  alpha_m, the mean over %d rows (relative error sqrt(1 - a^2) / a):\n   ", r->n);
	for (m = 1; m <= NPLANES; m++)
	{
		double		a = asum[m] / r->n;

		printf(" %d: %.5f (%.4f)", m, a, sqrt(Max(1.0 - a * a, 0.0)) / a);
	}
	printf("\n   alpha1, the signs', %.5f; the codes' top bit not the sign bit in %ld of %ld dimensions\n",
		   a1sum / r->n, r->topbit_off, (long) r->n * r->D);

	query_alloc(&Q, r->D);
	for (j = 0; j < nq; j++)
	{
		query_init(&Q, metric, mean, qx + (Size) j * r->D, 1.0);
		for (i = 0; i < r->n; i++)
		{
			double		ip = 0.0;
			double		e8;
			double		err8;
			int			d;

			for (d = 0; d < r->D; d++)
				ip += (double) r->xc[(Size) i * r->D + d] * Q.qd[d];
			prefix_estimates(r, i, &Q, &P);
			lionvec_estimate8(&Q.q, r->codes + (Size) i * r->D, r->norm[i],
							  r->alpha[(Size) i * 9 + 8], r->rnorm[(Size) i * 9 + 8],
							  &e8, &err8);
			maxdiff = Max(maxdiff, fabs(P.estf[8] - e8) / Max(fabs(err8), 1e-30));
			for (m = 2; m <= NPLANES; m++)
			{
				int			s;

				for (s = 1; s <= 3; s++)
				{
					outf[m][s - 1] += fabs(P.estf[m] - ip) > s * P.errf[m];
					outq[m][s - 1] += fabs(P.estq[m] - ip) > s * P.errq[m];
				}
			}
			n++;
		}
	}
	query_free(&Q);
	printf("  the prefix's sum at 8 planes against lionvec_estimate8(): at most %.2e of its own error\n",
		   maxdiff);
	CHECK(maxdiff < 1e-6, "eight planes are not the codes' estimate");
	printf("  outside 1, 2, 3 sd (the Gaussian's 0.3173, 0.0455, 0.0027), floats | %d-bit query:\n",
		   QB_BITS);
	for (m = 2; m <= NPLANES; m++)
	{
		printf("    m = %d: %.4f %.4f %.5f | %.4f %.4f %.5f\n", m,
			   (double) outf[m][0] / n, (double) outf[m][1] / n, (double) outf[m][2] / n,
			   (double) outq[m][0] / n, (double) outq[m][1] / n, (double) outq[m][2] / n);
		/* a light tail is fine, a heavy one is not */
		CHECK((double) outf[m][2] / n < 0.006 && (double) outq[m][2] / n < 0.006,
			  "a prefix's bound fails too often at 3 sd");
		CHECK((double) outf[m][0] / n < 0.40 && (double) outq[m][0] / n < 0.40,
			  "a prefix's bound is miscalibrated at 1 sd");
	}
}

/* ---------- 2. the planes a refine needs ---------- */

/*
 * The variants of the bound: at planes 2 to 7, LIONVEC_EPS8(eps) wide as the
 * codes' is, or eps wide as plane 1's; at plane 8 always the codes' own,
 * LIONVEC_EPS8(eps).  And the sums from the floats or from the quantized
 * query.
 */
enum
{
	V_F_WIDE, V_F_NARROW, V_Q_WIDE, V_Q_NARROW, NVARIANTS
};
static const char *const variant_names[NVARIANTS] = {
	"floats, 2 eps", "floats, eps", "quantized, 2 eps", "quantized, eps"
};

typedef struct Tally
{
	long		refined;		/* plane 1's lower bound below the k-th key */
	long		today_out;		/* ... the codes' lower bound past it */
	long		at[NVARIANTS][NPLANES + 2];		/* decided past it at plane m; 9: none */
	long		lost[NVARIANTS];	/* of the k nearest, decided past */
	long		lost_today;
	long		planes_read[NVARIANTS];	/* planes 2 .. m read */
} Tally;

static double
part_key(int metric, double lo, double offset, double margin)
{
	if (isinf(lo))
		return lo;
	if (metric == LIONVEC_L2)
	{
		double		d = sqrt(Max(lo + offset, 0.0)) - margin;

		return (d > 0.0 ? d * d : 0.0) - offset;
	}
	return lo - margin;
}

/*
 * One row (or centroid, margin > 0) against the k-th key kth: tallied if
 * plane 1 leaves it below; true key truek, to count what is lost.
 */
static void
tally_one(Tally *t, const Rows *r, int i, const Query *Q, int metric,
		  double kth, double truek, double margin)
{
	Prefix		P;
	double		est;
	double		err;
	double		lo;
	double		hi;
	int			v;
	int			m;
	bool		near = truek <= kth;

	lionvec_estimate1(&Q->q, r->bits + (Size) i * r->nw, r->pop[i], r->norm[i],
					  r->alpha1[i], &est, &err);
	lionvec_key_bounds(metric, r->norm[i], r->xm[i], est, err, &lo, &hi);
	if (part_key(metric, lo, Q->q.offset, margin) > kth)
		return;
	t->refined++;
	prefix_estimates(r, i, Q, &P);

	/* today: the codes, 2 eps */
	lionvec_key_bounds(metric, r->norm[i], r->xm[i], P.estf[8],
					   LIONVEC_EPS8(EPS) * P.errf[8], &lo, &hi);
	if (part_key(metric, lo, Q->q.offset, margin) > kth)
	{
		t->today_out++;
		t->lost_today += near;
	}

	for (v = 0; v < NVARIANTS; v++)
	{
		bool		quant = v == V_Q_WIDE || v == V_Q_NARROW;
		bool		wide = v == V_F_WIDE || v == V_Q_WIDE;

		for (m = 2; m <= NPLANES; m++)
		{
			double		w = (m == NPLANES || wide) ? LIONVEC_EPS8(EPS) : EPS;

			lionvec_key_bounds(metric, r->norm[i], r->xm[i],
							   quant ? P.estq[m] : P.estf[m],
							   w * (quant ? P.errq[m] : P.errf[m]), &lo, &hi);
			if (part_key(metric, lo, Q->q.offset, margin) > kth)
				break;
		}
		t->at[v][m]++;
		t->planes_read[v] += Min(m, NPLANES) - 1;
		if (m <= NPLANES)
			t->lost[v] += near;
	}
}

static void
tally_print(const char *what, const Tally *t, int nq, int D)
{
	int			v;
	int			m;

	printf("    %s: %.1f a query below the k-th by plane 1; the codes put %.1f%% of them past it today",
		   what, (double) t->refined / nq,
		   t->refined > 0 ? 100.0 * t->today_out / t->refined : 0.0);
	if (t->lost_today > 0)
		printf(" (%ld of the nearest lost)", t->lost_today);
	printf("\n");
	if (t->refined == 0)
		return;
	for (v = 0; v < NVARIANTS; v++)
	{
		double		cum = 0.0;
		double		mean = (double) t->planes_read[v] / t->refined;

		printf("      %-17s past it by plane", variant_names[v]);
		for (m = 2; m <= NPLANES; m++)
		{
			cum += t->at[v][m];
			printf(" %d: %4.1f%%", m, 100.0 * cum / t->refined);
		}
		printf("; %.2f planes, %.0f of %d bytes", mean, mean * D / 8, D);
		if (t->lost[v] > 0)
			printf("; %ld nearest lost", t->lost[v]);
		printf("\n");
	}
}

/* the nearest of k centroids to x, by l2sq, and the squared distance */
static int
nearest(const float *x, const float *cent, int k, int D, double *dist)
{
	double		best = INFINITY;
	int			bc = 0;
	int			c;

	for (c = 0; c < k; c++)
	{
		double		dd = lionvec_l2sq(x, cent + (Size) c * D, D);

		if (dd < best)
		{
			best = dd;
			bc = c;
		}
	}
	*dist = best;
	return bc;
}

#define KMEANS_TRAIN	32768

/*
 * Partitions for the centroids: Lloyd's k-means, by exact distances, of a
 * strided sample of at most KMEANS_TRAIN of the prepared rows (rotated, not
 * centred), from k of them, iters iterations, spherical for cosine, as the
 * build's is; then every row to its nearest centroid, into assign[n].  The
 * centroids into cent[k][D]; returns the rows' scale, the root of their
 * mean squared distance from their centroids over the dimensions, as the
 * meta page keeps it.  No spill: a row is in its nearest partition alone.
 */
static double
kmeans(const float *x, int n, int D, int dims, int k, int iters, bool unit,
	   float *cent, int *assign)
{
	int			nt = Min(n, KMEANS_TRAIN);
	int		   *ta = (int *) xmalloc(sizeof(int) * nt);
	double	   *sum = (double *) xmalloc(sizeof(double) * (Size) k * D);
	int		   *cnt = (int *) xmalloc(sizeof(int) * k);
	double		sumsq = 0.0;
	int			it;
	int			i;
	int			c;
	int			d;

#define TRAIN_ROW(t)	(x + (Size) ((Size) (t) * n / nt) * D)
	for (c = 0; c < k; c++)
		memcpy(cent + (Size) c * D, TRAIN_ROW((Size) c * nt / k), sizeof(float) * D);
	for (it = 0; it < iters; it++)
	{
		double		dd;

		for (i = 0; i < nt; i++)
			ta[i] = nearest(TRAIN_ROW(i), cent, k, D, &dd);
		memset(sum, 0, sizeof(double) * k * D);
		memset(cnt, 0, sizeof(int) * k);
		for (i = 0; i < nt; i++)
		{
			const float *xi = TRAIN_ROW(i);

			cnt[ta[i]]++;
			for (d = 0; d < D; d++)
				sum[(Size) ta[i] * D + d] += xi[d];
		}
		for (c = 0; c < k; c++)
		{
			double		n2 = 0.0;

			if (cnt[c] == 0)
				continue;
			for (d = 0; d < D; d++)
				n2 += (sum[(Size) c * D + d] / cnt[c]) * (sum[(Size) c * D + d] / cnt[c]);
			for (d = 0; d < D; d++)
				cent[(Size) c * D + d] = (float) (sum[(Size) c * D + d] / cnt[c] /
												  (unit && n2 > 0.0 ? sqrt(n2) : 1.0));
		}
	}
#undef TRAIN_ROW
	for (i = 0; i < n; i++)
	{
		double		dd;

		assign[i] = nearest(x + (Size) i * D, cent, k, D, &dd);
		sumsq += dd;
	}
	free(ta);
	free(sum);
	free(cnt);
	return sqrt(sumsq / n / dims);
}

/*
 * The rows raw[n][dims] and queries qraw[nq][dims], under metric: the
 * planes their refines need.  Rows are refined if plane 1 leaves them below
 * the k-th row's key, of every row (an exhaustive scan's) and of the rows
 * in the partitions a scan by the margin alone probes - those whose
 * centroid's codes, less the margin, leave it below the k-th, no pause -
 * and centroids if plane 1, less the margin, leaves them below it.
 */
static void
eval_planes(const char *name, const float *raw, int n, const float *qraw, int nq,
			int dims, int metric, bool centroids)
{
	int			D = LIONVEC_PADDED(dims);
	float	   *prep = falloc((Size) n * D);
	float	   *qprep = falloc((Size) nq * D);
	float	   *mean = falloc(D);
	double	   *msum = (double *) xmalloc(sizeof(double) * D);
	double	   *keys = (double *) xmalloc(sizeof(double) * n);
	double	   *sorted = (double *) xmalloc(sizeof(double) * n);
	int		   *assign = (int *) xmalloc(sizeof(int) * n);
	float	   *rot_signs;
	bool	   *probed = NULL;
	LionVecRotation rot;
	Rows		R;
	Rows		C;
	Query		Q;
	Tally		tr;
	Tally		tp;
	Tally		tc;
	int			nparts = centroids ? (int) (LIONVEC_PARTS_PER_ROOT * sqrt((double) n)) : 0;
	double		margin = 0.0;
	double		nprobed = 0.0;
	double		t0 = now_ns();
	int			i;
	int			j;
	int			d;

	memset(&tr, 0, sizeof(tr));
	memset(&tp, 0, sizeof(tp));
	memset(&tc, 0, sizeof(tc));
	rot_signs = falloc(lionvec_rotation_nsigns(D));
	lionvec_rotation_init(&rot, D, 4242, rot_signs);
	memset(msum, 0, sizeof(double) * D);
	for (i = 0; i < n; i++)
	{
		if (!lionvec_prepare(&rot, metric, raw + (Size) i * dims, dims, prep + (Size) i * D))
			memset(prep + (Size) i * D, 0, sizeof(float) * D);
		for (d = 0; d < D; d++)
			msum[d] += prep[(Size) i * D + d];
	}
	for (d = 0; d < D; d++)
		mean[d] = (float) (msum[d] / n);
	for (j = 0; j < nq; j++)
		(void) lionvec_prepare(&rot, metric, qraw + (Size) j * dims, dims, qprep + (Size) j * D);

	if (nparts > 0)
	{
		float	   *cent = falloc((Size) nparts * D);

		margin = MARGIN * kmeans(prep, n, D, dims, nparts, 3, metric == LIONVEC_COSINE,
								 cent, assign);
		rows_alloc(&C, nparts, D);
		for (i = 0; i < nparts; i++)
			rows_encode(&C, i, mean, cent + (Size) i * D);
		free(cent);
		probed = (bool *) xmalloc(sizeof(bool) * nparts);
	}
	rows_alloc(&R, n, D);
	for (i = 0; i < n; i++)
		rows_encode(&R, i, mean, prep + (Size) i * D);
	free(prep);

	query_alloc(&Q, D);
	for (j = 0; j < nq; j++)
	{
		double		kth;

		query_init(&Q, metric, mean, qprep + (Size) j * D, EPS);
		for (i = 0; i < n; i++)
		{
			const float *xc = R.xc + (Size) i * D;
			double		p[4] = {0};
			double		lo;
			double		hi;

			for (d = 0; d < D; d += 4)
			{
				p[0] += (double) xc[d] * Q.qd[d];
				p[1] += (double) xc[d + 1] * Q.qd[d + 1];
				p[2] += (double) xc[d + 2] * Q.qd[d + 2];
				p[3] += (double) xc[d + 3] * Q.qd[d + 3];
			}
			lionvec_key_bounds(metric, R.norm[i], R.xm[i], (p[0] + p[1]) + (p[2] + p[3]),
							   0.0, &lo, &hi);
			keys[i] = sorted[i] = lo;
		}
		qsort(sorted, n, sizeof(double), cmp_double);
		kth = sorted[K_NEAREST - 1];

		for (i = 0; i < nparts; i++)
		{
			double		est;
			double		err;
			double		lo;
			double		hi;

			lionvec_estimate8(&Q.q, C.codes + (Size) i * D, C.norm[i],
							  C.alpha[(Size) i * 9 + 8], C.rnorm[(Size) i * 9 + 8], &est, &err);
			lionvec_key_bounds(metric, C.norm[i], C.xm[i], est, err, &lo, &hi);
			probed[i] = part_key(metric, lo, Q.q.offset, margin) <= kth;
			nprobed += probed[i];
			tally_one(&tc, &C, i, &Q, metric, kth, INFINITY, margin);
		}
		for (i = 0; i < n; i++)
		{
			tally_one(&tr, &R, i, &Q, metric, kth, keys[i], 0.0);
			if (nparts > 0 && probed[assign[i]])
				tally_one(&tp, &R, i, &Q, metric, kth, keys[i], 0.0);
		}
	}
	printf("  %s, %d dimensions, metric %d: %d rows, %d queries, the %d nearest (%.1f s)\n",
		   name, dims, metric, n, nq, K_NEAREST, (now_ns() - t0) / 1e9);
	tally_print("every row", &tr, nq, D);
	if (nparts > 0)
	{
		printf("    %d partitions, margin %.4f (%.1f standard deviations), %.1f probed a query\n",
			   nparts, margin, MARGIN, nprobed / nq);
		tally_print("rows of the probed partitions", &tp, nq, D);
		tally_print("centroids", &tc, nq, D);
		rows_free(&C);
		free(probed);
	}
	query_free(&Q);
	rows_free(&R);
	free(qprep);
	free(mean);
	free(msum);
	free(keys);
	free(sorted);
	free(assign);
	free(rot_signs);
}

/* vec_test's kinds of data, n rows and nq queries */
static void
eval_kind(int kind, int dims, int metric, int n, int nq, bool centroids)
{
	float	   *raw = falloc((Size) n * dims);
	float	   *qraw = falloc((Size) nq * dims);

	make_data(kind, raw, n, qraw, nq, dims);
	eval_planes(data_names[kind], raw, n, qraw, nq, dims, metric, centroids);
	free(raw);
	free(qraw);
}

/* n vectors of dims float4s from path, raw: the whole file, or -1 */
static float *
read_vectors(const char *path, int dims, int *n)
{
	FILE	   *f = fopen(path, "rb");
	long		len;
	float	   *v;

	if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
		len % ((long) sizeof(float) * dims) != 0 || fseek(f, 0, SEEK_SET) != 0)
	{
		printf("%s: not a file of %d-dimension float4 vectors\n", path, dims);
		exit(2);
	}
	*n = (int) (len / ((long) sizeof(float) * dims));
	v = falloc((Size) *n * dims);
	if (fread(v, sizeof(float) * dims, *n, f) != (size_t) *n)
	{
		printf("%s: short read\n", path);
		exit(2);
	}
	fclose(f);
	return v;
}


/* ---------- 3. what a plane costs ---------- */

static volatile double sink;

#define TIME_ROUNDS		9
#define POOL_BYTES		(24 << 20)	/* past a core's L2, within the L3 */
#define COLD_ROWS		2048

static inline void
flush_lines(const void *p, Size len)
{
#ifdef LION_HAVE_X86_SIMD
	const char *c = (const char *) p;
	Size		o;

	for (o = 0; o < len; o += 64)
		_mm_clflush(c + o);
#endif
}

/* one refine at a time, as a scan's are: each waits on the queue it came from */
static inline void
fence(void)
{
#ifdef LION_HAVE_X86_SIMD
	_mm_mfence();
	_mm_lfence();
#endif
}

enum
{
	T_SUM8, T_DOT4, T_QUANT, T_FLOAT, T_INT16
};

enum
{
	M_WARM, M_POOL, M_FLUSHED, NMODES
};

/*
 * One refine's sums: the codes' (T_SUM8), or planes 2 .. upto, popcounts
 * of the quantized query (T_QUANT) or masked float sums (T_FLOAT); T_DOT4,
 * plane 1's own popcounts, for scale.
 */
static inline double
one_refine(int what, const uint8 *codes, const uint64 *planes, int upto,
		   const Query *Q, int D)
{
	int			nw = D / 64;
	double		s = 0.0;
	int			p;

	switch (what)
	{
		case T_SUM8:
			return lionvec_sum8(codes, Q->q.qc, D);
		case T_DOT4:
			return (double) lionvec_plane_dot(planes, Q->q.planes, nw);
		case T_QUANT:
			for (p = 2; p <= upto; p++)
				s += (double) lion_planes_dot_interleaved(planes + (p - 1) * nw, Q->qb,
														  (uint32) nw, QB_BITS);
			return s;
		case T_INT16:
			return (double) idot16(codes, Q->qw, D);
		case T_FLOAT:
			for (p = 2; p <= upto; p++)
				s += psum(planes + (p - 1) * nw, Q->qd, nw);
			return s;
	}
	return s;
}

/*
 * ns a refine, the median of TIME_ROUNDS rounds: warm, the same row again
 * and again for about 2 ms; pool, every row of the pool once, in a random
 * order, each refine fenced from the next; flushed, COLD_ROWS rows of it,
 * the lines each refine reads flushed first, less the time of the flushes.
 */
static double
time_refine(int what, int mode, const Rows *R, int upto, const Query *Q,
			const int *order)
{
	double		rounds[TIME_ROUNDS];
	int			D = R->D;
	int			nw = R->nw;
	int			nrows = mode == M_FLUSHED ? Min(COLD_ROWS, R->n) : R->n;
	int			r;

	for (r = 0; r < TIME_ROUNDS; r++)
	{
		double		t0;
		double		t1;
		double		t2;
		long		reps;
		long		i;
		int			pass;

		if (mode == M_WARM)
		{
			t0 = now_ns();
			sink += one_refine(what, R->codes, R->planes, upto, Q, D);
			reps = Max(1, (long) (2e6 / Max(now_ns() - t0, 1.0)));
			t0 = now_ns();
			for (i = 0; i < reps; i++)
				sink += one_refine(what, R->codes, R->planes, upto, Q, D);
			rounds[r] = (now_ns() - t0) / reps;
			continue;
		}
		t0 = t1 = t2 = 0.0;
		for (pass = 0; pass < 2; pass++)
		{
			if (pass == 1 && mode != M_FLUSHED)
				break;
			if (pass == 0)
				t0 = now_ns();
			else
				t1 = now_ns();
			for (i = 0; i < nrows; i++)
			{
				int			row = order[i];
				const uint8 *c = R->codes + (Size) row * D;
				const uint64 *pl = R->planes + (Size) row * NPLANES * nw;

				if (mode == M_FLUSHED)
				{
					if (what == T_SUM8 || what == T_INT16)
						flush_lines(c, D);
					else if (what != T_DOT4)
						flush_lines(pl + nw, sizeof(uint64) * nw * (upto - 1));
				}
				fence();
				if (pass == 0)
					sink += one_refine(what, c, pl, upto, Q, D);
				fence();
			}
			if (pass == 0)
				t1 = now_ns();
			else
				t2 = now_ns();
		}
		rounds[r] = mode == M_FLUSHED ?
			((t1 - t0) - (t2 - t1)) / nrows : (t1 - t0) / nrows;
	}
	qsort(rounds, TIME_ROUNDS, sizeof(double), cmp_double);
	return rounds[TIME_ROUNDS / 2];
}

static void
eval_costs(int D)
{
	static const struct
	{
		int			what;
		int			upto;
		const char *name;
	}			calls[] = {
		{T_SUM8, 8, "lionvec_sum8()"},
		{T_DOT4, 1, "plane 1 (4-bit query)"},
		{T_QUANT, 2, "quantized, plane 2"},
		{T_QUANT, 3, "quantized, planes 2-3"},
		{T_QUANT, 4, "quantized, planes 2-4"},
		{T_QUANT, 5, "quantized, planes 2-5"},
		{T_QUANT, 6, "quantized, planes 2-6"},
		{T_QUANT, 8, "quantized, planes 2-8"},
		{T_INT16, 8, "bytes, 16-bit query"},
		{T_FLOAT, 2, "floats, plane 2"},
		{T_FLOAT, 4, "floats, planes 2-4"},
		{T_FLOAT, 8, "floats, planes 2-8"},
	};
	int			nw = D / 64;
	int			n = POOL_BYTES / D;
	float	   *rot_signs = falloc(lionvec_rotation_nsigns(D));
	LionVecRotation rot;
	Rows		R;
	Query		Q;
	float	   *x = falloc(D);
	int		   *order = (int *) xmalloc(sizeof(int) * n);
	double		res[LIONVEC_KERNELS_MAX + 1][lengthof(calls)][NMODES];
	bool		have[LIONVEC_KERNELS_MAX + 1];
	int			impl;
	int			c;
	int			i;
	int			d;

	lionvec_rotation_init(&rot, D, 77, rot_signs);
	rows_alloc(&R, n, D);
	for (i = 0; i < n; i++)
	{
		for (d = 0; d < D; d++)
			x[d] = (float) rnd_gauss();
		lionvec_rotate(&rot, x);
		rows_encode(&R, i, NULL, x);
		order[i] = i;
	}
	for (i = n - 1; i > 0; i--)
	{
		int			j = (int) (rnd64() % (i + 1));
		int			t = order[i];

		order[i] = order[j];
		order[j] = t;
	}
	for (d = 0; d < D; d++)
		x[d] = (float) rnd_gauss();
	lionvec_rotate(&rot, x);
	query_alloc(&Q, D);

	for (impl = LIONVEC_KERNELS_PORTABLE; impl <= LIONVEC_KERNELS_MAX; impl++)
	{
		have[impl] = lionvec_kernel_force(impl);
		if (!have[impl])
			continue;
		query_init(&Q, LIONVEC_L2, NULL, x, EPS);

		/* the masked sums agree to the bit with the portable one */
		for (i = 0; i < Min(n, 64); i++)
		{
			int			p;

			for (p = 1; p <= NPLANES; p++)
			{
				const uint64 *pl = R.planes + ((Size) i * NPLANES + p - 1) * nw;

				CHECK(psum(pl, Q.qd, nw) == psum_portable(pl, Q.qd, nw),
					  "a masked sum is not the portable one's");
			}
			CHECK(idot16(R.codes + (Size) i * D, Q.qw, D) ==
				  idot16_portable(R.codes + (Size) i * D, Q.qw, D),
				  "an integer dot is not the portable one's");
		}
		for (c = 0; c < (int) lengthof(calls); c++)
		{
			int			mode;

			for (mode = 0; mode < NMODES; mode++)
				res[impl][c][mode] = (calls[c].what == T_DOT4 && mode == M_FLUSHED) ? NAN :
					time_refine(calls[c].what, mode, &R, calls[c].upto, &Q, order);
		}
	}
	lionvec_kernels_init();

	printf("  %d dimensions (a pool of %d rows, %d MB): ns a refine, warm / pool / flushed\n",
		   D, n, POOL_BYTES >> 20);
	printf("    %-24s", "");
	for (impl = LIONVEC_KERNELS_PORTABLE; impl <= LIONVEC_KERNELS_MAX; impl++)
		if (have[impl])
			printf(" %22s", lion_simd_name((LionSimdImpl) (LION_SIMD_PORTABLE + impl)));
	printf("\n");
	for (c = 0; c < (int) lengthof(calls); c++)
	{
		printf("    %-24s", calls[c].name);
		for (impl = LIONVEC_KERNELS_PORTABLE; impl <= LIONVEC_KERNELS_MAX; impl++)
			if (have[impl])
				printf("   %6.0f /%6.0f /%6.0f", res[impl][c][M_WARM],
					   res[impl][c][M_POOL], res[impl][c][M_FLUSHED]);
		printf("\n");
	}
	query_free(&Q);
	rows_free(&R);
	free(rot_signs);
	free(x);
	free(order);
}


/* 1. on clustered rows of 384 and 1536 dimensions, cosine */
static void
eval_prefixes_all(bool quick)
{
	static const int pdims[] = {384, 1536};
	int			t;

	printf("1. the prefixes (clustered rows, cosine)\n");
	for (t = 0; t < (int) lengthof(pdims); t++)
	{
		int			dims = pdims[t];
		int			D = LIONVEC_PADDED(dims);
		int			pn = quick ? 500 : 2000;
		int			pq = quick ? 4 : 12;
		float	   *raw = falloc((Size) pn * dims);
		float	   *qraw = falloc((Size) pq * dims);
		float	   *prep = falloc((Size) D);
		float	   *qprep = falloc((Size) pq * D);
		float	   *mean = falloc(D);
		float	   *rs = falloc(lionvec_rotation_nsigns(D));
		LionVecRotation rot;
		Rows		R;
		int			i;
		int			d;

		printf(" %d dimensions:\n", dims);
		lionvec_rotation_init(&rot, D, 4242, rs);
		make_data(DATA_CLUSTERS, raw, pn, qraw, pq, dims);
		memset(mean, 0, sizeof(float) * D);
		rows_alloc(&R, pn, D);
		for (i = 0; i < pn; i++)
		{
			(void) lionvec_prepare(&rot, LIONVEC_COSINE, raw + (Size) i * dims, dims, prep);
			for (d = 0; d < D; d++)
				mean[d] += prep[d] / pn;
		}
		for (i = 0; i < pn; i++)
		{
			(void) lionvec_prepare(&rot, LIONVEC_COSINE, raw + (Size) i * dims, dims, prep);
			rows_encode(&R, i, mean, prep);
		}
		for (i = 0; i < pq; i++)
			(void) lionvec_prepare(&rot, LIONVEC_COSINE, qraw + (Size) i * dims, dims,
								   qprep + (Size) i * D);
		eval_prefixes(&R, mean, qprep, pq, LIONVEC_COSINE);
		rows_free(&R);
		free(raw);
		free(qraw);
		free(prep);
		free(qprep);
		free(mean);
		free(rs);
	}
}

/*
 *	vecplanes_eval [--quick]		all three parts, on vec_test's kinds of data
 *	vecplanes_eval --costs			part 3 alone
 *	vecplanes_eval --file ROWS QUERIES DIMS
 *									part 2 on the vectors of two files of raw
 *									float4s (cosine), as DESIGN.md §44.14 ran it
 *									on DBpedia's rows
 */
int
main(int argc, char **argv)
{
	bool		quick = argc > 1 && strcmp(argv[1], "--quick") == 0;
	int			n = quick ? 4000 : 20000;
	int			nq = quick ? 10 : 40;
	int			kind;

	setvbuf(stdout, NULL, _IOLBF, 0);
	lionvec_tables_init();
	lionvec_kernels_init();
	printf("vecplanes_eval: best implementation %s\n", lion_simd_name(lion_simd_best()));

	if (argc == 5 && strcmp(argv[1], "--file") == 0)
	{
		int			dims = atoi(argv[4]);
		int			nr;
		int			nqf;
		float	   *rows;
		float	   *queries;

		if (dims < 1 || dims > 16000)
		{
			printf("bad dimensions %s\n", argv[4]);
			return 2;
		}
		rows = read_vectors(argv[2], dims, &nr);
		queries = read_vectors(argv[3], dims, &nqf);
		printf("2. the planes a refine needs (eps %.0f; plane 8 at 2 eps, as the codes are)\n", EPS);
		eval_planes(argv[2], rows, nr, queries, nqf, dims, LIONVEC_COSINE, true);
		free(rows);
		free(queries);
	}
	else if (argc == 2 && strcmp(argv[1], "--costs") == 0)
	{
		printf("3. what a refine's sums cost\n");
		eval_costs(384);
		eval_costs(1536);
	}
	else if (argc == 1 || quick)
	{
		eval_prefixes_all(quick);
		printf("2. the planes a refine needs (eps %.0f; plane 8 at 2 eps, as the codes are)\n", EPS);
		for (kind = 0; kind < NDATA; kind++)
		{
			eval_kind(kind, 384, LIONVEC_COSINE, n, nq, true);
			eval_kind(kind, 1536, LIONVEC_COSINE, n, nq, true);
		}
		eval_kind(DATA_CLUSTERS, 1536, LIONVEC_L2, n, nq, false);
		printf("3. what a refine's sums cost\n");
		eval_costs(384);
		eval_costs(1536);
	}
	else
	{
		printf("usage: vecplanes_eval [--quick | --costs | --file ROWS QUERIES DIMS]\n");
		return 2;
	}
	printf("vecplanes_eval: %ld checks failed\n", nfail);
	return nfail > 0 ? 1 : 0;
}
