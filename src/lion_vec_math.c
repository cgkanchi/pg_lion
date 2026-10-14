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

/* x86-64 GCC and clang build a POPCNT kernel beside the portable one (§3) */
#if !defined(LION_NO_SIMD) && defined(__x86_64__) && \
	((defined(__clang__) && __clang_major__ >= 6) || \
	 (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 5))
#define LIONVEC_HAVE_POPCNT 1
#define LIONVEC_TARGET(isa)	__attribute__((target(isa)))
#endif

/* the portable kernel's popcount, as lion_container.c chooses it */
#if (defined(__GNUC__) || defined(__clang__)) && \
	(defined(__POPCNT__) || !(defined(__x86_64__) || defined(__i386__)))
#define lionvec_popcount64(w)	((uint32) __builtin_popcountll(w))
#else
#define lionvec_popcount64(w)	((uint32) pg_popcount64(w))
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

/* Rotate x[0 .. dimsp) in place. */
void
lionvec_rotate(const LionVecRotation *rot, float *x)
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
	int			i;

	Assert(dimsp >= 64 && dimsp % 64 == 0);
	q->qnorm2 = 0.0;
	for (i = 0; i < dimsp; i++)
	{
		double		m = mean != NULL ? mean[i] : 0.0;

		q->qc[i] = (float) (x[i] - m);
		mq += m * x[i];
		q->qnorm2 += (double) q->qc[i] * (double) q->qc[i];
		mn = Min(mn, (double) q->qc[i]);
		mx = Max(mx, (double) q->qc[i]);
	}

	q->qlo = mn;
	q->qdelta = mx > mn ? (mx - mn) / levels : 1.0;
	q->qsum = 0.0;
	memset(q->planes, 0, sizeof(uint64) * (dimsp / 64) * LIONVEC_QBITS);
	for (i = 0; i < dimsp; i++)
	{
		double		t = (q->qc[i] - q->qlo) / q->qdelta + 0.5;
		int			u = t >= levels ? levels : (t > 0.0 ? (int) t : 0);
		double		qq = q->qlo + q->qdelta * u;
		int			j;

		for (j = 0; j < LIONVEC_QBITS; j++)
			if (u & (1 << j))
				q->planes[(i / 64) * LIONVEC_QBITS + j] |= UINT64CONST(1) << (i % 64);
		q->qsum += qq;
		e2 += (q->qc[i] - qq) * (q->qc[i] - qq);
	}

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
 * Plane 1 against the query's planes
 * ----------------------------------------------------------------
 */

static pg_always_inline uint32
lionvec_dot_words(const uint64 *bits, const uint64 *planes, int nwords,
				  bool hw)
{
	uint32		s0 = 0;
	uint32		s1 = 0;
	uint32		s2 = 0;
	uint32		s3 = 0;
	int			w;

	StaticAssertStmt(LIONVEC_QBITS == 4, "the kernels unroll four planes");
	for (w = 0; w < nwords; w++)
	{
		uint64		b = bits[w];
		const uint64 *p = planes + w * LIONVEC_QBITS;

#ifdef LIONVEC_HAVE_POPCNT
		if (hw)
		{
			s0 += (uint32) __builtin_popcountll(b & p[0]);
			s1 += (uint32) __builtin_popcountll(b & p[1]);
			s2 += (uint32) __builtin_popcountll(b & p[2]);
			s3 += (uint32) __builtin_popcountll(b & p[3]);
			continue;
		}
#endif
		s0 += lionvec_popcount64(b & p[0]);
		s1 += lionvec_popcount64(b & p[1]);
		s2 += lionvec_popcount64(b & p[2]);
		s3 += lionvec_popcount64(b & p[3]);
	}
	return s0 + 2 * s1 + 4 * s2 + 8 * s3;
}

static uint32
lionvec_dot_portable(const uint64 *bits, const uint64 *planes, int nwords)
{
	return lionvec_dot_words(bits, planes, nwords, false);
}

#ifdef LIONVEC_HAVE_POPCNT
static LIONVEC_TARGET("popcnt") uint32
lionvec_dot_popcnt(const uint64 *bits, const uint64 *planes, int nwords)
{
	return lionvec_dot_words(bits, planes, nwords, true);
}
#endif

LionVecDotKernel lionvec_plane_dot = lionvec_dot_portable;

void
lionvec_kernels_init(void)
{
#ifdef LIONVEC_HAVE_POPCNT
	if (__builtin_cpu_supports("popcnt"))
		lionvec_plane_dot = lionvec_dot_popcnt;
#endif
}

#ifdef FRONTEND
bool
lionvec_kernel_force(int which)
{
	if (which == 0)
	{
		lionvec_plane_dot = lionvec_dot_portable;
		return true;
	}
#ifdef LIONVEC_HAVE_POPCNT
	if (which == 1 && __builtin_cpu_supports("popcnt"))
	{
		lionvec_plane_dot = lionvec_dot_popcnt;
		return true;
	}
#endif
	return false;
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

int
lionvec_candidates_n(const LionVecCentroids *c, const LionVecQuery *q, int m,
					 double *lo, double *hi, int *cand)
{
	int			nw = c->dimsp / 64;
	double		least[LIONVEC_RANK_MAX];	/* the m least upper bounds, ascending */
	int			nleast = 0;
	double		thr;
	int			n = 0;
	int			j;

	Assert(c->dimsp == q->dimsp);
	Assert(m >= 1 && m <= LIONVEC_RANK_MAX);
	m = Max(1, Min(m, LIONVEC_RANK_MAX));
	for (j = 0; j < c->k; j++)
	{
		double		est;
		double		err;
		int			i;

		lionvec_estimate1(q, c->bits + (Size) j * nw, c->popcount[j],
						  c->norm[j], c->alpha1[j], &est, &err);
		lionvec_key_bounds(LIONVEC_L2, c->norm[j], 0.0, est, err, &lo[j], &hi[j]);
		/* NaN bounds come back infinite, so every comparison here is total */
		if (nleast == m && !(hi[j] < least[m - 1]))
			continue;
		i = nleast < m ? nleast++ : m - 1;
		for (; i > 0 && least[i - 1] > hi[j]; i--)
			least[i] = least[i - 1];
		least[i] = hi[j];
	}
	/* fewer centroids than m: every one can be among the m nearest */
	thr = nleast == m ? least[m - 1] : INFINITY;
	for (j = 0; j < c->k; j++)
	{
		if (lo[j] <= thr)
			cand[n++] = j;
	}
	/* at least min(k, m) unless a bound came out with lo > hi (the header) */
	return n;
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

static double
lionvec_l2sq(const float *a, const float *b, int n)
{
	double		s = 0.0;
	int			i;

	for (i = 0; i < n; i++)
	{
		double		d = (double) a[i] - (double) b[i];

		s += d * d;
	}
	return s;
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

/* how far apart the halves of a split centroid start, as FAISS has it */
#define LIONVEC_SPLIT_EPS		(1.0f / 1024.0f)

int
lionvec_kmeans(const LionVecSample *s, int k, int iters, bool unit,
			   uint64 seed, const float *mean, float *cent,
			   LionVecKmeansWork *w)
{
	int			dimsp = s->dimsp;
	int			nw = dimsp / 64;
	int			n = s->n;
	pg_prng_state st;
	LionVecCentroids cs;
	LionVecQuery q;
	int			made = 0;
	int			i;
	int			j;

	Assert(dimsp >= 64 && dimsp % 64 == 0);
	Assert(k >= 1 && k <= n);

	/* k distinct rows to start from: a partial Fisher-Yates shuffle */
	pg_prng_seed(&st, seed);
	for (i = 0; i < n; i++)
		w->assign[i] = i;
	for (j = 0; j < k; j++)
	{
		int			r = j + (int) pg_prng_uint64_range(&st, 0, (uint64) (n - 1 - j));
		int			t = w->assign[j];

		w->assign[j] = w->assign[r];
		w->assign[r] = t;
		lionvec_sample_row(s, w->assign[j], cent + (Size) j * dimsp);
		if (unit)
			lionvec_unit(cent + (Size) j * dimsp, dimsp);
	}
	for (i = 0; i < n; i++)
		w->assign[i] = -1;

	cs.k = k;
	cs.dimsp = dimsp;
	cs.bits = w->bits;
	cs.popcount = w->popcount;
	cs.norm = w->norm;
	cs.alpha1 = w->alpha1;
	q.dimsp = dimsp;
	q.qc = w->qc;
	q.planes = w->planes;

	while (made < iters)
	{
		int			moved = 0;

		made++;
		/* the centroids' codes, which the candidates are found by */
		for (j = 0; j < k; j++)
		{
			LionVecEncoded e;

			memcpy(w->x, cent + (Size) j * dimsp, sizeof(float) * dimsp);
			e.bits = w->bits + (Size) j * nw;
			e.codes = w->codes;
			lionvec_encode(mean, dimsp, w->x, &e);
			w->popcount[j] = e.popcount;
			w->norm[j] = e.norm;
			w->alpha1[j] = e.alpha1;
		}

		/* each row to its nearest */
		memset(w->count, 0, sizeof(int) * k);
		for (i = 0; i < n; i++)
		{
			int			nc;
			int			best = -1;
			double		bestd = INFINITY;
			int			c;

			lionvec_sample_row(s, i, w->y);
			lionvec_query_init(&q, LIONVEC_L2, mean, w->y, LIONVEC_ASSIGN_EPS);
			nc = lionvec_candidates(&cs, &q, w->lo, w->hi, w->cand);
			for (c = 0; c < nc; c++)
			{
				double		d = lionvec_l2sq(w->y, cent + (Size) w->cand[c] * dimsp,
											 dimsp);

				/* in index order, so that ties go the same way every time */
				if (best < 0 || d < bestd)
				{
					best = w->cand[c];
					bestd = d;
				}
			}
			Assert(best >= 0);
			if (w->assign[i] != best)
				moved++;
			w->assign[i] = best;
			w->count[best]++;
		}
		/* no row moved: the centroids are already their rows' means */
		if (moved == 0)
			break;

		/* the means, summed where the centroids were: they are not needed now */
		for (j = 0; j < k; j++)
		{
			if (w->count[j] > 0)
				memset(cent + (Size) j * dimsp, 0, sizeof(float) * dimsp);
		}
		for (i = 0; i < n; i++)
		{
			float	   *sum = cent + (Size) w->assign[i] * dimsp;
			int			d;

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
	return made;
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
	double		s = 0.0;
	double		a = Max((double) alpha8, 1e-6);
	double		miss = Max(1.0 - a * a, 0.0);
	int			i;

	for (i = 0; i < q->dimsp; i++)
		s += (double) lionvec_recon8[codes[i]] * (double) q->qc[i];
	*est = rnorm8 > 0.0f ? norm * s / (rnorm8 * a) : 0.0;
	*err = LIONVEC_EPS8(q->eps) * norm / a * sqrt(q->var1 * miss);
}
