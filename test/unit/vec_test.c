/*-------------------------------------------------------------------------
 * vec_test.c
 *	  Standalone unit tests for src/lion_vec_math.c: lionvec's rotation,
 *	  codes and estimates (DESIGN.md §44.3 - §44.4).
 *
 *	  Built by "make unit PG_CONFIG=..." with -DFRONTEND.  The properties
 *	  under test:
 *
 *	  - the 8-bit codes: even steps, symmetric, lionvec_code8() the inverse
 *		of lionvec_recon8[] and never more than half a step off;
 *	  - the rotation: orthogonal (norms and inner products kept) at every
 *		shape of window, the same from the same seed, and spreading a basis
 *		vector into Gaussian-looking coordinates;
 *	  - a row's codes: the bits are the signs, the popcount theirs, the alphas
 *		and norms what their definitions say, a row at the mean all zeros;
 *	  - the query's planes: they quantize it within half a step, and the sum
 *		and variance kept with them are the planes' own;
 *	  - every popcount kernel against a naive count;
 *	  - the key algebra: with the exact inner product and no error, the
 *		bounds ARE the key of the distance computed the obvious way, for each
 *		metric - which is the proof that centring, the mean's terms, cosine's
 *		normalization and the query's offset fit together;
 *	  - the estimates' error bounds hold as often as the Gaussian says they
 *		should, on clustered data of several sizes: at one standard deviation
 *		about a third of the time, at three almost always.
 *
 *	  Every array is an exact-size heap allocation, which `make unit
 *	  SANITIZE=1` checks to the byte.  Exit status is 0 only if every check
 *	  passed.
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
			printf("FAIL [%s] vec_test.c:%d: %s\n", cur_phase, line, msg);
		else if (nfail == 41)
			printf("... further failures suppressed\n");
	}
}

#define CHECK(ok, msg)	check_impl((ok), __LINE__, (msg))

/* A small deterministic generator, so that a failure reproduces. */
static uint64 rng_state = 0x9E3779B97F4A7C15ULL;

static uint64
rnd64(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

/* uniform in (0, 1) */
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
falloc(int n)
{
	return (float *) xmalloc(sizeof(float) * n);
}

static double
dot_ff(const float *a, const float *b, int n)
{
	double		s = 0.0;
	int			i;

	for (i = 0; i < n; i++)
		s += (double) a[i] * (double) b[i];
	return s;
}

/* A rotation of dimsp dimensions, its signs exactly as long as they must be. */
static void
make_rotation(LionVecRotation *rot, int dimsp, uint32 seed)
{
	float	   *signs = falloc((int) lionvec_rotation_nsigns(dimsp));

	lionvec_rotation_init(rot, dimsp, seed, signs);
}

/* ---------- the 8-bit bins ---------- */

static void
test_tables(void)
{
	int			k;
	double		m1 = 0.0;
	bool		ok;

	cur_phase = "tables";
	lionvec_tables_init();
	lionvec_tables_init();		/* idempotent */

	ok = true;
	for (k = 0; k < 256; k++)
	{
		if (k > 0)
			ok &= lionvec_recon8[k] - lionvec_recon8[k - 1] == 1.0f / LIONVEC_STEPS8;
		ok &= lionvec_recon8[k] == -lionvec_recon8[255 - k];
		m1 += lionvec_recon8[k];
	}
	CHECK(ok, "reconstructions not evenly spaced or not symmetric");
	CHECK(m1 == 0.0, "reconstructions do not average to 0");
	CHECK(lionvec_recon8[0] == -LIONVEC_CLIP8 + 0.5f / LIONVEC_STEPS8 &&
		  lionvec_recon8[255] == LIONVEC_CLIP8 - 0.5f / LIONVEC_STEPS8, "the ends");

	ok = true;
	for (k = 0; k < 256; k++)
	{
		float		lo = -LIONVEC_CLIP8 + (float) k / LIONVEC_STEPS8;

		ok &= lionvec_code8(lionvec_recon8[k]) == k;
		/* either side of a step's edge (exactly at it, float rounding decides) */
		if (k > 0)
			ok &= lionvec_code8(lo + 1e-4f) == k &&
				lionvec_code8(lo - 1e-4f) == k - 1;
	}
	CHECK(ok, "code8 is not the inverse of recon8, or a step's edge is in the wrong step");
	CHECK(lionvec_code8(-1e30f) == 0 && lionvec_code8(1e30f) == 255, "extremes");
	CHECK(lionvec_code8(-INFINITY) == 0 && lionvec_code8(INFINITY) == 255, "infinities");
	CHECK(lionvec_code8(NAN) == 0, "a NaN does not get code 0");
	{
		uint8		prev = 0;
		double		z;

		ok = true;
		for (z = -5.0; z <= 5.0; z += 0.001)
		{
			uint8		c = lionvec_code8((float) z);

			ok &= c >= prev;
			ok &= fabs(z) >= LIONVEC_CLIP8 || fabs(lionvec_recon8[c] - z) <= 0.5 / LIONVEC_STEPS8 + 1e-6;
			prev = c;
		}
		CHECK(ok, "code8 is not monotone, or its reconstruction more than half a step off");
	}
}

/* ---------- the rotation ---------- */

static const int test_dims[] = {64, 128, 192, 256, 320, 1536, 2048, 3072, 8128};

static void
test_rotation(void)
{
	int			t;

	cur_phase = "rotation";
	for (t = 0; t < (int) lengthof(test_dims); t++)
	{
		int			D = test_dims[t];
		LionVecRotation rot;
		LionVecRotation rot2;
		LionVecRotation rot3;
		float	   *x = falloc(D);
		float	   *y = falloc(D);
		float	   *rx = falloc(D);
		float	   *ry = falloc(D);
		double		maxrel = 0.0;
		double		maxip = 0.0;
		int			trial;
		int			i;
		bool		same;
		bool		differ;
		double		mx;

		make_rotation(&rot, D, 12345);
		make_rotation(&rot2, D, 12345);
		make_rotation(&rot3, D, 54321);

		/* the windows cover the vector, at both ends */
		CHECK(rot.block <= D && D < 2 * rot.block, "window not the largest power of two");
		CHECK((rot.block & (rot.block - 1)) == 0, "window not a power of two");
		CHECK(rot.win[0] == 0 && rot.win[1] + rot.block == D, "windows do not reach both ends");
		CHECK(rot.nwin == (rot.block == D ? 1 : 2), "window count");
		CHECK(lionvec_rotation_nsigns(D) == (Size) LIONVEC_ROUNDS * rot.nwin * D, "nsigns");

		same = memcmp(rot.signs, rot2.signs, sizeof(float) * lionvec_rotation_nsigns(D)) == 0;
		differ = memcmp(rot.signs, rot3.signs, sizeof(float) * lionvec_rotation_nsigns(D)) != 0;
		CHECK(same, "same seed, different signs");
		CHECK(differ, "different seed, same signs");
		{
			Size		n = lionvec_rotation_nsigns(D);
			Size		k;
			Size		pos = 0;

			for (k = 0; k < n; k++)
				pos += rot.signs[k] > 0;
			CHECK(pos > n * 2 / 5 && pos < n * 3 / 5, "signs not balanced");
		}

		for (trial = 0; trial < 20; trial++)
		{
			double		nx;
			double		ny;
			double		ip;

			for (i = 0; i < D; i++)
			{
				x[i] = (float) rnd_gauss();
				y[i] = (float) (rnd_gauss() + 0.5 * x[i]);
			}
			memcpy(rx, x, sizeof(float) * D);
			memcpy(ry, y, sizeof(float) * D);
			lionvec_rotate(&rot, rx);
			lionvec_rotate(&rot, ry);
			nx = sqrt(dot_ff(x, x, D));
			ny = sqrt(dot_ff(y, y, D));
			ip = dot_ff(x, y, D);
			maxrel = Max(maxrel, fabs(sqrt(dot_ff(rx, rx, D)) - nx) / nx);
			maxip = Max(maxip, fabs(dot_ff(rx, ry, D) - ip) / (nx * ny));
		}
		CHECK(maxrel < 1e-5, "rotation changed a norm");
		CHECK(maxip < 1e-5, "rotation changed an inner product");

		/* the same seed, the same rotation */
		memcpy(rx, x, sizeof(float) * D);
		memcpy(ry, x, sizeof(float) * D);
		lionvec_rotate(&rot, rx);
		lionvec_rotate(&rot2, ry);
		CHECK(memcmp(rx, ry, sizeof(float) * D) == 0, "same seed, different rotation");

		/* a basis vector comes out spread: no coordinate much over 1/sqrt(D) */
		memset(x, 0, sizeof(float) * D);
		x[D / 3] = 1.0f;
		lionvec_rotate(&rot, x);
		mx = 0.0;
		for (i = 0; i < D; i++)
			mx = Max(mx, fabs(x[i]));
		CHECK(mx * sqrt((double) D) < 6.0, "a basis vector was not spread");

		free(x);
		free(y);
		free(rx);
		free(ry);
		free(rot.signs);
		free(rot2.signs);
		free(rot3.signs);
	}

	/*
	 * The coordinates of rotated unit vectors, times sqrt(D), look like a
	 * standard Gaussian: what the error model assumes.  Unit vectors along
	 * one axis, the hardest case for a structured rotation.
	 */
	{
		int			D = 1536;
		LionVecRotation rot;
		float	   *x = falloc(D);
		double		s2 = 0.0;
		double		s4 = 0.0;
		long		tail = 0;
		long		n = 0;
		int			b;
		int			i;

		make_rotation(&rot, D, 7);
		for (b = 0; b < D; b += 7)
		{
			memset(x, 0, sizeof(float) * D);
			x[b] = 1.0f;
			lionvec_rotate(&rot, x);
			for (i = 0; i < D; i++)
			{
				double		z = x[i] * sqrt((double) D);

				s2 += z * z;
				s4 += z * z * z * z;
				tail += fabs(z) > 3.0;
				n++;
			}
		}
		CHECK(fabs(s2 / n - 1.0) < 1e-3, "rotated coordinates' variance is not 1/D");
		CHECK(fabs(s4 / n - 3.0) < 0.3, "rotated coordinates' kurtosis is not a Gaussian's");
		CHECK((double) tail / n > 0.0010 && (double) tail / n < 0.0045,
			  "rotated coordinates' tail beyond 3 is not a Gaussian's");
		free(x);
		free(rot.signs);
	}
}

/* ---------- preparing a vector ---------- */

static void
test_prepare(void)
{
	int			dims = 300;
	int			D = LIONVEC_PADDED(dims);
	LionVecRotation rot;
	float	   *v = falloc(dims);
	float	   *x = falloc(D);
	int			i;
	double		n2 = 0.0;

	cur_phase = "prepare";
	CHECK(D == 320, "padding");
	CHECK(LIONVEC_PADDED(1) == 64 && LIONVEC_PADDED(64) == 64 &&
		  LIONVEC_PADDED(65) == 128, "padding edges");
	make_rotation(&rot, D, 99);

	for (i = 0; i < dims; i++)
	{
		v[i] = (float) (rnd_gauss() * 3.0);
		n2 += (double) v[i] * v[i];
	}
	CHECK(lionvec_prepare(&rot, LIONVEC_L2, v, dims, x), "L2 refused a vector");
	CHECK(fabs(dot_ff(x, x, D) - n2) / n2 < 1e-5, "L2: padding and rotation changed the norm");
	CHECK(lionvec_prepare(&rot, LIONVEC_COSINE, v, dims, x), "cosine refused a vector");
	CHECK(fabs(dot_ff(x, x, D) - 1.0) < 1e-5, "cosine: not unit length");

	memset(v, 0, sizeof(float) * dims);
	CHECK(lionvec_prepare(&rot, LIONVEC_L2, v, dims, x), "L2 refused a zero vector");
	CHECK(dot_ff(x, x, D) == 0.0, "L2: zero vector not zero");
	CHECK(lionvec_prepare(&rot, LIONVEC_IP, v, dims, x), "IP refused a zero vector");
	CHECK(!lionvec_prepare(&rot, LIONVEC_COSINE, v, dims, x), "cosine took a zero vector");

	free(v);
	free(x);
	free(rot.signs);
}

/* ---------- a row's codes ---------- */

typedef struct EncBufs
{
	LionVecEncoded e;
	uint64	   *bits;
	uint8	   *codes;
} EncBufs;

static void
enc_alloc(EncBufs *b, int D)
{
	b->bits = (uint64 *) xmalloc(sizeof(uint64) * (D / 64));
	b->codes = (uint8 *) xmalloc(D);
	b->e.bits = b->bits;
	b->e.codes = b->codes;
}

static void
enc_free(EncBufs *b)
{
	free(b->bits);
	free(b->codes);
}

static void
test_encode(void)
{
	int			D = 768;
	float	   *mean = falloc(D);
	float	   *x = falloc(D);
	float	   *xc = falloc(D);
	EncBufs		b;
	int			trial;
	int			i;

	cur_phase = "encode";
	enc_alloc(&b, D);
	for (i = 0; i < D; i++)
		mean[i] = (float) (rnd_gauss() * 0.1);

	for (trial = 0; trial < 50; trial++)
	{
		double		norm;
		double		s1 = 0.0;
		double		rr = 0.0;
		double		rx = 0.0;
		uint32		pop = 0;
		bool		bitsok = true;
		double		xm;

		for (i = 0; i < D; i++)
		{
			x[i] = (float) (mean[i] + rnd_gauss() * 0.05 * (1 + trial % 5));
			xc[i] = x[i] - mean[i];
		}
		xm = dot_ff(xc, mean, D);
		norm = sqrt(dot_ff(xc, xc, D));
		lionvec_encode(mean, D, x, &b.e);

		CHECK(memcmp(x, xc, sizeof(float) * D) == 0, "encode did not centre x in place");
		CHECK(fabs(b.e.norm - norm) / norm < 1e-6, "norm");
		CHECK(fabs(b.e.xm - xm) < 1e-5 * (1 + fabs(xm)), "xm");
		for (i = 0; i < D; i++)
		{
			bool		bit = (b.bits[i / 64] >> (i % 64)) & 1;
			double		r = lionvec_recon8[b.codes[i]];

			bitsok &= bit == (xc[i] > 0.0f);
			pop += bit;
			s1 += fabs(xc[i]);
			rr += r * r;
			rx += r * xc[i];
			bitsok &= b.codes[i] == lionvec_code8((float) (xc[i] * sqrt((double) D) / norm));
		}
		CHECK(bitsok, "bits are not the signs, or codes not the bins");
		CHECK(b.e.popcount == pop, "popcount");
		CHECK(fabs(b.e.alpha1 - s1 / (norm * sqrt((double) D))) < 1e-6, "alpha1");
		CHECK(b.e.alpha1 > 0.74 && b.e.alpha1 < 0.85, "alpha1 is not sqrt(2/pi)-ish for Gaussian coordinates");
		CHECK(fabs(b.e.rnorm8 - sqrt(rr)) < 1e-4 * sqrt(rr), "rnorm8");
		CHECK(fabs(b.e.alpha8 - rx / (sqrt(rr) * norm)) < 1e-6, "alpha8");
		/* a 32nd of a standard deviation: 1 - alpha8^2 is about 1e-4 */
		CHECK(b.e.alpha8 > 0.99990 && b.e.alpha8 <= 1.0f, "alpha8 not close to 1");
	}

	/* a row at the mean */
	memcpy(x, mean, sizeof(float) * D);
	lionvec_encode(mean, D, x, &b.e);
	CHECK(b.e.norm == 0.0f && b.e.alpha1 == 1.0f && b.e.alpha8 == 1.0f &&
		  b.e.popcount == 0, "a row at the mean");
	{
		bool		ok = true;

		for (i = 0; i < D / 64; i++)
			ok &= b.bits[i] == 0;
		for (i = 0; i < D; i++)
			ok &= b.codes[i] == 128;
		CHECK(ok, "a row at the mean has codes");
	}

	/* no mean: nothing is subtracted */
	for (i = 0; i < D; i++)
		x[i] = xc[i] = (float) rnd_gauss();
	lionvec_encode(NULL, D, x, &b.e);
	CHECK(memcmp(x, xc, sizeof(float) * D) == 0 && b.e.xm == 0.0f, "no mean");

	enc_free(&b);
	free(mean);
	free(x);
	free(xc);
}

/* ---------- the query's planes ---------- */

static void
query_alloc(LionVecQuery *q, int D)
{
	q->dimsp = D;
	q->qc = falloc(D);
	q->planes = (uint64 *) xmalloc(sizeof(uint64) * (D / 64) * LIONVEC_QBITS);
}

static void
query_free(LionVecQuery *q)
{
	free(q->qc);
	free(q->planes);
}

static void
test_query(void)
{
	int			D = 512;
	float	   *mean = falloc(D);
	float	   *x = falloc(D);
	LionVecQuery q;
	int			i;
	int			metric;

	cur_phase = "query";
	query_alloc(&q, D);
	for (i = 0; i < D; i++)
	{
		mean[i] = (float) (rnd_gauss() * 0.2);
		x[i] = (float) (mean[i] + rnd_gauss());
	}

	for (metric = 1; metric <= LIONVEC_NSTRATEGIES; metric++)
	{
		double		qsum = 0.0;
		double		e2 = 0.0;
		double		mq = dot_ff(mean, x, D);
		bool		ok = true;

		lionvec_query_init(&q, metric, mean, x, 3.0);
		for (i = 0; i < D; i++)
		{
			int			u = 0;
			int			j;
			double		qq;

			for (j = 0; j < LIONVEC_QBITS; j++)
				u |= (int) ((q.planes[(i / 64) * LIONVEC_QBITS + j] >> (i % 64)) & 1) << j;
			qq = q.qlo + q.qdelta * u;
			ok &= q.qc[i] == (float) (x[i] - mean[i]);
			ok &= fabs(qq - q.qc[i]) <= q.qdelta / 2 + 1e-6;
			qsum += qq;
			e2 += (q.qc[i] - qq) * (q.qc[i] - qq);
		}
		CHECK(ok, "a query coordinate is off its plane value by more than half a step");
		CHECK(fabs(q.qsum - qsum) < 1e-6 * (1 + fabs(qsum)), "qsum is not the planes'");
		CHECK(fabs(q.varq - e2 / D) < 1e-9, "varq is not the planes' error");
		CHECK(fabs(q.qnorm2 - dot_ff(q.qc, q.qc, D)) < 1e-6 * q.qnorm2, "qnorm2");
		CHECK(fabs(q.var1 - q.qnorm2 / (D - 1)) < 1e-12, "var1");
		CHECK(q.eps == 3.0, "eps");
		if (metric == LIONVEC_L2)
			CHECK(q.offset == q.qnorm2, "L2 offset");
		else if (metric == LIONVEC_IP)
			CHECK(fabs(q.offset - mq) < 1e-6 * (1 + fabs(mq)), "IP offset");
		else
			CHECK(fabs(q.offset - (mq - 1.0)) < 1e-6 * (1 + fabs(mq)), "cosine offset");
	}

	/* a query at the mean: one level, nothing lost to rounding */
	lionvec_query_init(&q, LIONVEC_L2, mean, mean, 3.0);
	CHECK(q.qdelta == 1.0 && q.qlo == 0.0 && q.varq == 0.0 && q.qnorm2 == 0.0,
		  "a query at the mean");
	{
		bool		ok = true;

		for (i = 0; i < (D / 64) * LIONVEC_QBITS; i++)
			ok &= q.planes[i] == 0;
		CHECK(ok, "a query at the mean has bits");
	}

	query_free(&q);
	free(mean);
	free(x);
}

/* ---------- the popcount kernels ---------- */

static uint32
naive_dot(const uint64 *bits, const uint64 *planes, int nwords)
{
	uint32		s = 0;
	int			w;
	int			j;
	int			b;

	for (w = 0; w < nwords; w++)
		for (j = 0; j < LIONVEC_QBITS; j++)
			for (b = 0; b < 64; b++)
				if (((bits[w] & planes[w * LIONVEC_QBITS + j]) >> b) & 1)
					s += 1u << j;
	return s;
}

static void
test_kernels(void)
{
	int			which;
	int			ran = 0;

	cur_phase = "kernels";
	for (which = 0; which <= 1; which++)
	{
		int			nwords;

		if (!lionvec_kernel_force(which))
			continue;
		ran++;
		for (nwords = 1; nwords <= 127; nwords += (nwords < 8 ? 1 : 13))
		{
			uint64	   *bits = (uint64 *) xmalloc(sizeof(uint64) * nwords);
			uint64	   *planes = (uint64 *) xmalloc(sizeof(uint64) * nwords * LIONVEC_QBITS);
			int			trial;
			int			i;
			bool		ok = true;

			for (trial = 0; trial < 20; trial++)
			{
				for (i = 0; i < nwords; i++)
					bits[i] = rnd64() & (trial % 3 == 0 ? rnd64() : ~UINT64CONST(0));
				for (i = 0; i < nwords * LIONVEC_QBITS; i++)
					planes[i] = rnd64();
				ok &= lionvec_plane_dot(bits, planes, nwords) == naive_dot(bits, planes, nwords);
			}
			/* everything set: the largest sum a kernel adds up */
			for (i = 0; i < nwords; i++)
				bits[i] = ~UINT64CONST(0);
			for (i = 0; i < nwords * LIONVEC_QBITS; i++)
				planes[i] = ~UINT64CONST(0);
			ok &= lionvec_plane_dot(bits, planes, nwords) == (uint32) (nwords * 64 * 15);
			CHECK(ok, which == 0 ? "portable kernel differs from the naive count" :
				  "POPCNT kernel differs from the naive count");
			free(bits);
			free(planes);
		}
	}
	CHECK(ran >= 1, "no kernel ran");
	printf("vec_test: %d popcount kernel(s) tested\n", ran);
	lionvec_kernels_init();
}

/* ---------- the key algebra, and the bounds' coverage ---------- */

/*
 * Clustered data in dims dimensions: n rows and nq queries around k centres,
 * as embeddings are - neighbours much nearer than the rest.
 */
static void
make_clusters(float *rows, int n, float *queries, int nq, int dims, int k)
{
	float	   *centres = falloc(k * dims);
	int			i;
	int			d;

	for (i = 0; i < k * dims; i++)
		centres[i] = (float) (rnd_gauss() / sqrt((double) dims) + 0.02);
	for (i = 0; i < n; i++)
	{
		int			c = (int) (rnd64() % k);

		for (d = 0; d < dims; d++)
			rows[i * dims + d] = (float) (centres[c * dims + d] +
										  0.6 * rnd_gauss() / sqrt((double) dims));
	}
	for (i = 0; i < nq; i++)
	{
		int			c = (int) (rnd64() % k);

		for (d = 0; d < dims; d++)
			queries[i * dims + d] = (float) (centres[c * dims + d] +
											 0.7 * rnd_gauss() / sqrt((double) dims));
	}
	free(centres);
}

static double
true_distance(int metric, const float *a, const float *b, int dims)
{
	double		s = 0.0;
	double		na = 0.0;
	double		nb = 0.0;
	int			d;

	for (d = 0; d < dims; d++)
	{
		if (metric == LIONVEC_L2)
			s += ((double) a[d] - b[d]) * ((double) a[d] - b[d]);
		else
			s += (double) a[d] * b[d];
		na += (double) a[d] * a[d];
		nb += (double) b[d] * b[d];
	}
	if (metric == LIONVEC_L2)
		return sqrt(s);
	if (metric == LIONVEC_IP)
		return -s;
	return 1.0 - s / sqrt(na * nb);
}

typedef struct Coverage
{
	long		n;
	long		out1_eps1;		/* plane 1's bound missed at eps = 1 */
	long		out1;			/* ... at the eps tested */
	long		out8_eps1;
	long		out8;
	double		width1;			/* mean err of plane 1 over the key's spread */
} Coverage;

static void
coverage(int dims, int metric, int n, int nq, double eps, Coverage *cov)
{
	int			D = LIONVEC_PADDED(dims);
	float	   *rows = falloc(n * dims);
	float	   *queries = falloc(nq * dims);
	float	   *x = falloc(D);
	float	   *mean = falloc(D);
	double	   *sum = (double *) xmalloc(sizeof(double) * D);
	LionVecRotation rot;
	EncBufs		b;
	LionVecQuery q;
	LionVecQuery q1;
	float	   *prepped = falloc(n * D);
	LionVecEncoded *enc = (LionVecEncoded *) xmalloc(sizeof(LionVecEncoded) * n);
	uint64	   *bits = (uint64 *) xmalloc(sizeof(uint64) * (D / 64) * n);
	uint8	   *codes = (uint8 *) xmalloc((Size) D * n);
	int			i;
	int			j;
	long		algebra_bad = 0;
	double		wsum = 0.0;

	memset(cov, 0, sizeof(*cov));
	make_clusters(rows, n, queries, nq, dims, 12);
	make_rotation(&rot, D, 4242);
	enc_alloc(&b, D);
	query_alloc(&q, D);
	query_alloc(&q1, D);

	/* the mean, as the build takes it: of the prepared rows */
	memset(sum, 0, sizeof(double) * D);
	for (i = 0; i < n; i++)
	{
		CHECK(lionvec_prepare(&rot, metric, rows + i * dims, dims, prepped + i * D),
			  "prepare refused a row");
		for (j = 0; j < D; j++)
			sum[j] += prepped[i * D + j];
	}
	for (j = 0; j < D; j++)
		mean[j] = (float) (sum[j] / n);

	for (i = 0; i < n; i++)
	{
		enc[i].bits = bits + (Size) i * (D / 64);
		enc[i].codes = codes + (Size) i * D;
		lionvec_encode(mean, D, prepped + i * D, &enc[i]);	/* centres it */
	}

	for (j = 0; j < nq; j++)
	{
		double		kmin = INFINITY;
		double		kmax = -INFINITY;

		CHECK(lionvec_prepare(&rot, metric, queries + j * dims, dims, x), "prepare refused a query");
		lionvec_query_init(&q, metric, mean, x, eps);
		lionvec_query_init(&q1, metric, mean, x, 1.0);
		for (i = 0; i < n; i++)
		{
			const float *xc = prepped + i * D;
			double		ip = dot_ff(xc, q.qc, D);
			double		key = lionvec_key_exact(metric,
												true_distance(metric, rows + i * dims,
															  queries + j * dims, dims),
												q.offset);
			double		est;
			double		err;
			double		lo;
			double		hi;

			/* the algebra: the exact inner product, no error, is the key */
			lionvec_key_bounds(metric, enc[i].norm, enc[i].xm, ip, 0.0, &lo, &hi);
			if (fabs(lo - key) > 1e-4 * (1.0 + fabs(key)) || lo != hi)
				algebra_bad++;

			lionvec_estimate1(&q, enc[i].bits, enc[i].popcount, enc[i].norm,
							  enc[i].alpha1, &est, &err);
			cov->out1 += fabs(est - ip) > err;
			lionvec_key_bounds(metric, enc[i].norm, enc[i].xm, est, err, &lo, &hi);
			wsum += hi - lo;
			kmin = Min(kmin, key);
			kmax = Max(kmax, key);
			lionvec_estimate1(&q1, enc[i].bits, enc[i].popcount, enc[i].norm,
							  enc[i].alpha1, &est, &err);
			cov->out1_eps1 += fabs(est - ip) > err;

			lionvec_estimate8(&q, enc[i].codes, enc[i].norm, enc[i].alpha8,
							  enc[i].rnorm8, &est, &err);
			cov->out8 += fabs(est - ip) > err;
			lionvec_estimate8(&q1, enc[i].codes, enc[i].norm, enc[i].alpha8,
							  enc[i].rnorm8, &est, &err);
			cov->out8_eps1 += fabs(est - ip) > err;
			cov->n++;
		}
		cov->width1 += wsum / n / Max(kmax - kmin, 1e-12);
		wsum = 0.0;
	}
	cov->width1 /= nq;
	CHECK(algebra_bad == 0, "the key of the exact inner product is not the key of the distance");

	enc_free(&b);
	query_free(&q);
	query_free(&q1);
	free(rot.signs);
	free(rows);
	free(queries);
	free(x);
	free(mean);
	free(sum);
	free(prepped);
	free(enc);
	free(bits);
	free(codes);
}

static void
test_coverage(void)
{
	static const int cdims[] = {100, 384, 1536};
	int			t;
	int			metric;

	cur_phase = "coverage";
	for (t = 0; t < (int) lengthof(cdims); t++)
	{
		for (metric = 1; metric <= LIONVEC_NSTRATEGIES; metric++)
		{
			Coverage	cov;
			double		r1;
			double		r1e1;
			double		r8;
			double		r8e1;

			coverage(cdims[t], metric, 2000, 12, 3.0, &cov);
			r1 = (double) cov.out1 / cov.n;
			r1e1 = (double) cov.out1_eps1 / cov.n;
			r8 = (double) cov.out8 / cov.n;
			r8e1 = (double) cov.out8_eps1 / cov.n;
			printf("vec_test: dims %4d metric %d: plane 1 outside 1 sd %.4f, 3 sd %.5f; "
				   "codes outside 2 sd %.4f, 6 sd %.6f; plane 1 width %.3f of the keys' spread\n",
				   cdims[t], metric, r1e1, r1, r8e1, r8, cov.width1);
			/* the Gaussian's 0.317 at one sd; a light tail is fine, a heavy one is not */
			CHECK(r1e1 > 0.10 && r1e1 < 0.40, "plane 1's bound is miscalibrated at 1 sd");
			CHECK(r1 < 0.004, "plane 1's bound fails too often at 3 sd");
			CHECK(r8e1 < 0.10, "the codes' bound is miscalibrated at 2 sd");
			CHECK(r8 < 0.0001, "the codes' bound fails at 6 sd");
		}
	}
}

/* ---------- the DIR chain's pages ---------- */

/*
 * A build's directory, made as the build makes it: rows in heap block order,
 * their offsets within a block in any order (a HOT chain is indexed under its
 * root's offset), cut into pages of up to cap rows.  rows[] gets every row's
 * TID key and page[] its page; returns the pages.
 */
static int
dir_build(LionVecDirEntry *dir, uint64 *rows, int *page, int nrows, int cap,
		  uint32 blk0)
{
	int			npages = 0;
	int			onpage = 0;
	int			r = 0;
	uint32		blk = blk0;

	while (r < nrows)
	{
		int			start = r;
		int			inblk = (int) (rnd64() % 12) + 1;
		int			k;

		for (k = 1; k <= inblk && r < nrows; k++, r++)
			rows[r] = LIONVEC_TID_KEY(blk, k);
		if (rnd64() % 3 == 0)
		{
			for (k = r - 1; k > start; k--)
			{
				int			o = start + (int) (rnd64() % (uint64) (k - start + 1));
				uint64		t = rows[k];

				rows[k] = rows[o];
				rows[o] = t;
			}
		}
		blk += (uint32) (rnd64() % 3 == 0 ? 1 + rnd64() % 5 : 1);
	}
	for (r = 0; r < nrows; r++)
	{
		/* a page fills to cap rows, or ends early now and then */
		if (r == 0 || onpage == cap || rnd64() % 8 == 0)
		{
			dir[npages].lo = dir[npages].hi = rows[r];
			npages++;
			onpage = 0;
		}
		else
		{
			dir[npages - 1].lo = Min(dir[npages - 1].lo, rows[r]);
			dir[npages - 1].hi = Max(dir[npages - 1].hi, rows[r]);
		}
		onpage++;
		page[r] = npages - 1;
	}
	return npages;
}

/*
 * lionvec_dir_mark against the definition, the ranges given over several
 * calls: a page is set iff its range meets one, so every row in a range lies
 * on a set page or on the last one.
 */
static void
check_dir_mark(const LionVecDirEntry *dir, int npages, const uint64 *lo,
			   const uint64 *hi, int nranges, const uint64 *rows,
			   const int *page, int nrows)
{
	int			nwords = Max((npages - 1 + 63) / 64, 0);
	uint64	   *sel = (uint64 *) xmalloc(sizeof(uint64) * (nwords + 1));
	int			from = 0;
	int			n = 0;
	int			done = 0;
	int			got = 0;
	int			i;
	int			j;

	memset(sel, 0, sizeof(uint64) * (nwords + 1));
	sel[nwords] = UINT64CONST(0x5a5a5a5a5a5a5a5a);	/* a canary past sel */
	while (done < nranges)
	{
		int			k = (int) (rnd64() % 4) + 1;

		k = Min(k, nranges - done);
		n += lionvec_dir_mark(dir, npages, lo + done, hi + done, k, &from, sel);
		done += k;
	}
	if (nranges == 0)
		n += lionvec_dir_mark(dir, npages, lo, hi, 0, &from, sel);
	CHECK(sel[nwords] == UINT64CONST(0x5a5a5a5a5a5a5a5a), "wrote past sel");
	if (npages >= 1 && (npages - 1) % 64 != 0)
		CHECK((sel[nwords - 1] >> ((npages - 1) % 64)) == 0, "a page past the last set");
	for (i = 0; i < npages - 1; i++)
	{
		bool		want = false;
		bool		set = (sel[i / 64] >> (i % 64)) & 1;

		for (j = 0; j < nranges && !want; j++)
			want = dir[i].lo <= hi[j] && dir[i].hi >= lo[j];
		CHECK(want == set, want ? "missed a page" : "an extra page");
		got += set;
	}
	CHECK(got == n, "the count is not the pages set");
	for (i = 0; i < nrows; i++)
	{
		bool		in = false;

		for (j = 0; j < nranges && !in; j++)
			in = rows[i] >= lo[j] && rows[i] <= hi[j];
		if (in && page[i] < npages - 1)
			CHECK((sel[page[i] / 64] >> (page[i] % 64)) & 1, "a member's page not set");
	}
	free(sel);
}

static void
test_dir_pages(void)
{
	static LionVecDirEntry dir[400];
	static uint64 rows[400];
	static int	page[400];
	uint64		lo[64];
	uint64		hi[64];
	int			trial;

	cur_phase = "dir";

	/* too few pages to list any: the tail is the caller's */
	dir[0].lo = LIONVEC_TID_KEY(7, 1);
	dir[0].hi = LIONVEC_TID_KEY(9, 3);
	dir[1].lo = LIONVEC_TID_KEY(9, 4);
	dir[1].hi = LIONVEC_TID_KEY(12, 1);
	lo[0] = 0;
	hi[0] = PG_UINT64_MAX;
	check_dir_mark(dir, 0, lo, hi, 1, rows, page, 0);
	check_dir_mark(dir, 1, lo, hi, 1, rows, page, 0);
	check_dir_mark(dir, 2, lo, hi, 1, rows, page, 0);	/* page 0 */
	lo[0] = hi[0] = LIONVEC_TID_KEY(9, 4);
	check_dir_mark(dir, 2, lo, hi, 1, rows, page, 0);	/* none: page 0 ends at (9,3) */
	lo[0] = hi[0] = LIONVEC_TID_KEY(9, 3);
	check_dir_mark(dir, 2, lo, hi, 1, rows, page, 0);	/* page 0, by its last row */
	lo[0] = LIONVEC_TID_KEY(0, 0);
	hi[0] = LIONVEC_TID_KEY(7, 0);
	check_dir_mark(dir, 2, lo, hi, 1, rows, page, 0);	/* none: page 0 starts at (7,1) */
	check_dir_mark(dir, 2, lo, hi, 0, rows, page, 0);	/* no ranges */
	CHECK(lionvec_dir_check(dir, 2), "a directory in order");

	/* the far end of the block numbers */
	dir[0].lo = LIONVEC_TID_KEY(PG_UINT32_MAX - 1, 1);
	dir[0].hi = LIONVEC_TID_KEY(PG_UINT32_MAX, 2);
	dir[1].lo = LIONVEC_TID_KEY(PG_UINT32_MAX, 3);
	dir[1].hi = LIONVEC_TID_KEY(PG_UINT32_MAX, 9);
	dir[2] = dir[1];
	lo[0] = LIONVEC_TID_KEY(PG_UINT32_MAX, 0);
	hi[0] = LIONVEC_TID_KEY(PG_UINT32_MAX, PG_UINT16_MAX);
	check_dir_mark(dir, 3, lo, hi, 1, rows, page, 0);
	CHECK(lionvec_dir_check(dir, 3), "the far end in order");

	/* out of order */
	dir[0].lo = LIONVEC_TID_KEY(5, 2);
	dir[0].hi = LIONVEC_TID_KEY(5, 1);
	CHECK(!lionvec_dir_check(dir, 1), "lo above hi");
	dir[0].lo = LIONVEC_TID_KEY(5, 1);
	dir[0].hi = LIONVEC_TID_KEY(6, 1);
	dir[1].lo = LIONVEC_TID_KEY(5, 9);
	dir[1].hi = LIONVEC_TID_KEY(7, 1);
	CHECK(!lionvec_dir_check(dir, 2), "a page's blocks before the last's");
	dir[1].lo = LIONVEC_TID_KEY(6, 0);
	CHECK(lionvec_dir_check(dir, 2), "pages sharing a block");

	/* at random: builds as the build makes them, ranges of every width */
	for (trial = 0; trial < 6000; trial++)
	{
		int			nrows = (int) (rnd64() % 400);
		int			cap = (int) (rnd64() % 20) + 1;
		int			npages = dir_build(dir, rows, page, nrows, cap,
									   (uint32) (rnd64() % 3));
		int			nranges = (int) (rnd64() % 64);
		uint64		x = LIONVEC_TID_KEY(rnd64() % 3, rnd64() % 4);
		uint64		span = LIONVEC_TID_KEY(rnd64() % 8, 0) + rnd64() % 16 + 1;
		int			j;

		CHECK(lionvec_dir_check(dir, npages), "a built directory out of order");
		for (j = 0; j < nranges; j++)
		{
			uint64		w = rnd64() % 4 == 0 ? rnd64() % span : 0;

			lo[j] = x;
			hi[j] = x + w;
			x = hi[j] + 1 + (rnd64() % 2 == 0 ? 0 : rnd64() % span);
		}
		check_dir_mark(dir, npages, lo, hi, nranges, rows, page, nrows);
	}
}

/* ---------- odd inputs ---------- */

static void
test_bounds_nan(void)
{
	double		lo;
	double		hi;
	int			metric;

	cur_phase = "nan";
	for (metric = 1; metric <= LIONVEC_NSTRATEGIES; metric++)
	{
		lionvec_key_bounds(metric, INFINITY, 0.0, INFINITY, INFINITY, &lo, &hi);
		CHECK(isinf(lo) && lo < 0 && isinf(hi) && hi > 0, "a NaN bound is not infinite");
		lionvec_key_bounds(metric, 1.0, 0.5, 0.25, 0.0, &lo, &hi);
		CHECK(lo == hi, "no error, no width");
		lionvec_key_bounds(metric, 1.0, 0.5, 0.25, 0.1, &lo, &hi);
		CHECK(lo < hi, "an error, a width");
	}
	CHECK(lionvec_key_exact(LIONVEC_L2, 3.0, 4.0) == 5.0, "L2 key");
	CHECK(lionvec_key_exact(LIONVEC_IP, -3.0, 4.0) == 1.0, "IP key");
	CHECK(lionvec_key_exact(LIONVEC_COSINE, 0.5, -0.25) == 0.25, "cosine key");
}

int
main(void)
{
	lionvec_tables_init();
	lionvec_kernels_init();

	test_tables();
	test_rotation();
	test_prepare();
	test_encode();
	test_query();
	test_kernels();
	test_bounds_nan();
	test_dir_pages();
	test_coverage();

	printf("vec_test: %ld checks, %ld failed\n", nchecks, nfail);
	return nfail == 0 ? 0 : 1;
}
