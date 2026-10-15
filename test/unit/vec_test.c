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
 *	  - every kernel the build and CPU have (portable, POPCNT, AVX2): plane
 *		1's against a naive count, the sums against their sixteen lanes
 *		added as the header says and the rotation against the portable
 *		one's, to the bit, at every dimsp an index takes and from unaligned
 *		pointers; and end to end, the same rotations, estimates and k-means
 *		as the portable set's;
 *	  - the key algebra: with the exact inner product and no error, the
 *		bounds ARE the key of the distance computed the obvious way, for each
 *		metric - which is the proof that centring, the mean's terms, cosine's
 *		normalization and the query's offset fit together;
 *	  - an inner product's partition bound (lionvec_lift_key()): a true bound
 *		at the lifted rows' radius, for rows of mixed lengths and unit rows,
 *		and the L2 bound on unit rows;
 *	  - spill: the ranking of candidates, the candidates of the m nearest
 *		centroids, a vector decoded from its 8-bit codes, and SOAR's choice of
 *		a second partition - its algebra on vectors whose answer is known,
 *		and on clustered rows the least cost, other than the second-nearest
 *		for a share of them, and the same from decoded centroids;
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

/*
 * The kernel tests draw from a generator of their own, swapped in and out,
 * so that what the tests after them draw does not depend on which kernels
 * the build and the CPU have.
 */
static uint64 kernel_rng_state = 0x2545F4914F6CDD1DULL;

static void
swap_rng(void)
{
	uint64		t = rng_state;

	rng_state = kernel_rng_state;
	kernel_rng_state = t;
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

/* ---------- the kernels ---------- */

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

static const char *const kernel_names[LIONVEC_KERNELS_MAX + 1] = {
	"portable", "POPCNT", "AVX2"
};

/* The most dimensions an index takes at 8K pages (lion_vec.h). */
#define TEST_MAX_DIMSP		8128

/*
 * What the sums' kernels must return, to the bit, stated apart from them:
 * the i-th term into partial i % 16, and the partials added up as the
 * header says.  serial is the plain sum in order, which it must be near.
 */
static double
lanes_reference(const double *terms, int n, double *serial)
{
	double		p[LIONVEC_LANES] = {0};
	double		u[4];
	int			i;

	*serial = 0.0;
	for (i = 0; i < n; i++)
	{
		p[i % LIONVEC_LANES] += terms[i];
		*serial += terms[i];
	}
	for (i = 0; i < 4; i++)
		u[i] = (p[i] + p[i + 8]) + (p[i + 4] + p[i + 12]);
	return (u[0] + u[2]) + (u[1] + u[3]);
}

/*
 * A float of a kind: Gaussian, tiny, denormal, huge, zero, or of any scale
 * from 2^-40 to 2^40 - which makes a sum's rounding depend on the order it
 * is added in, as sums of terms of one scale often do not.
 */
#define RND_KINDS	6
static float
rnd_float(int kind)
{
	switch (kind % RND_KINDS)
	{
		case 0:
			return (float) rnd_gauss();
		case 1:
			return (float) (rnd_gauss() * 1e-3);
		case 2:
			return (float) (rnd_gauss() * 1e-41);	/* denormal */
		case 3:
			return (float) (rnd_gauss() * 1e18);
		case 4:
			return (rnd64() & 1) ? 0.0f : -0.0f;
		default:
			return (float) ldexp(rnd_gauss(), (int) (rnd64() % 81) - 40);
	}
}

/*
 * Every kernel the build and CPU have, against a reference: plane 1's
 * against a bit-at-a-time count; the sums against lanes_reference(), to
 * the bit; the rotation against the portable kernel's, to the bit.  At
 * every dimsp an index takes, from pointers at every alignment the
 * kernels' loads could straddle.
 */
static void
test_kernels(void)
{
	int			which;
	int			ran = 0;
	char		names[128] = "";
	long		draws;

	cur_phase = "kernels";
	swap_rng();

	/* the 8-bit sums compute recon8[c] rather than load it: it must be exact */
	{
		bool		ok = true;
		int			k;

		for (k = 0; k < 256; k++)
			ok &= (double) lionvec_recon8[k] ==
				(double) k * (1.0 / LIONVEC_STEPS8) + (-LIONVEC_CLIP8 + 0.5 / LIONVEC_STEPS8);
		CHECK(ok, "recon8[c] is not c / 32 - 4 + 1/64 exactly");
	}

	for (which = 0; which <= LIONVEC_KERNELS_MAX; which++)
	{
		int			nwords;
		int			D;
		bool		dot_ok = true;
		bool		sum8_ok = true;
		bool		l2_ok = true;
		bool		l2_near = true;
		bool		sum8_near = true;
		bool		rot_ok = true;

		if (!lionvec_kernel_force(which))
		{
			printf("vec_test: no %s kernels in this build or on this CPU\n",
				   kernel_names[which]);
			continue;
		}
		ran++;
		snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s",
				 ran > 1 ? ", " : "", kernel_names[which]);

		/* plane 1, at every word count an index takes, one word off alignment */
		for (nwords = 1; nwords <= TEST_MAX_DIMSP / 64; nwords++)
		{
			uint64	   *bitsbuf = (uint64 *) xmalloc(sizeof(uint64) * (nwords + 1));
			uint64	   *planesbuf = (uint64 *) xmalloc(sizeof(uint64) * (nwords * LIONVEC_QBITS + 1));
			int			trial;
			int			i;

			for (trial = 0; trial < 4; trial++)
			{
				uint64	   *bits = bitsbuf + trial % 2;
				uint64	   *planes = planesbuf + trial / 2;

				for (i = 0; i < nwords; i++)
					bits[i] = rnd64() & (trial % 3 == 0 ? rnd64() : ~UINT64CONST(0));
				for (i = 0; i < nwords * LIONVEC_QBITS; i++)
					planes[i] = rnd64();
				dot_ok &= lionvec_plane_dot(bits, planes, nwords) == naive_dot(bits, planes, nwords);
			}
			/* everything set: the largest sum a kernel adds up */
			for (i = 0; i < nwords; i++)
				bitsbuf[i] = ~UINT64CONST(0);
			for (i = 0; i < nwords * LIONVEC_QBITS; i++)
				planesbuf[i] = ~UINT64CONST(0);
			dot_ok &= lionvec_plane_dot(bitsbuf, planesbuf, nwords) == (uint32) (nwords * 64 * 15);
			free(bitsbuf);
			free(planesbuf);
		}

		/* the sums and the rotation, at every dimsp */
		for (D = 64; D <= TEST_MAX_DIMSP; D += 64)
		{
			int			off = (D / 64) % 32;	/* bytes off a 32-byte boundary */
			int			foff = (D / 64) % 8;	/* floats off it */
			uint8	   *codesbuf = (uint8 *) xmalloc(D + 31);
			float	   *qcbuf = falloc(D + 7);
			float	   *bbuf = falloc(D + 7);
			double	   *terms = (double *) xmalloc(sizeof(double) * D);
			uint8	   *codes = codesbuf + off;
			float	   *qc = qcbuf + foff;
			float	   *b = bbuf + (7 - foff);
			int			kind = D / 64;
			double		ref;
			double		serial;
			double		got;
			int			i;

			for (i = 0; i < D; i++)
			{
				codes[i] = kind % 4 == 0 ? 0 : kind % 4 == 1 ? 255 : (uint8) rnd64();
				/* mostly of any scale; some of one kind; some mixed */
				qc[i] = rnd_float(kind % 3 == 0 ? (int) (rnd64() % RND_KINDS) :
								  kind % 3 == 1 ? kind : RND_KINDS - 1);
				b[i] = rnd_float(kind % 4 == 0 ? (int) (rnd64() % RND_KINDS) :
								 kind % 4 == 1 ? kind + 1 : RND_KINDS - 1);
			}

			for (i = 0; i < D; i++)
				terms[i] = (double) lionvec_recon8[codes[i]] * (double) qc[i];
			ref = lanes_reference(terms, D, &serial);
			got = lionvec_sum8(codes, qc, D);
			sum8_ok &= memcmp(&got, &ref, sizeof(double)) == 0;
			sum8_near &= fabs(got - serial) <= 1e-12 * (fabs(serial) + 1e-300) ||
				fabs(got - serial) <= 1e-9 * sqrt(dot_ff(qc, qc, D));

			for (i = 0; i < D; i++)
			{
				double		d = (double) qc[i] - (double) b[i];

				terms[i] = d * d;
			}
			ref = lanes_reference(terms, D, &serial);
			got = lionvec_l2sq(qc, b, D);
			l2_ok &= memcmp(&got, &ref, sizeof(double)) == 0;
			l2_near &= fabs(got - serial) <= 1e-12 * serial;

			/* the rotation, against the portable kernel's on the same vector */
			{
				LionVecRotation rot;
				float	   *x = qcbuf + (7 - foff);
				float	   *y = falloc(D);

				make_rotation(&rot, D, (uint32) D);
				for (i = 0; i < D; i++)
					x[i] = (float) rnd_gauss() * (i % 5 == 0 ? 1e-30f : 1.0f);
				memcpy(y, x, sizeof(float) * D);
				lionvec_rotate(&rot, x);
				lionvec_kernel_force(LIONVEC_KERNELS_PORTABLE);
				lionvec_rotate(&rot, y);
				lionvec_kernel_force(which);
				rot_ok &= memcmp(x, y, sizeof(float) * D) == 0;
				free(y);
				free(rot.signs);
			}
			free(codesbuf);
			free(qcbuf);
			free(bbuf);
			free(terms);
		}
		CHECK(dot_ok, "a plane-1 kernel differs from the naive count");
		CHECK(sum8_ok, "an 8-bit sum differs from the lanes' sum");
		CHECK(sum8_near, "an 8-bit sum is not near the plain sum");
		CHECK(l2_ok, "an l2sq differs from the lanes' sum");
		CHECK(l2_near, "an l2sq is not near the plain sum");
		CHECK(rot_ok, "a rotation differs from the portable one");
	}
	CHECK(ran >= 1, "no kernel ran");
	printf("vec_test: kernels tested: %s\n", names);
	lionvec_kernels_init();
	swap_rng();

	/*
	 * The statistical checks after this test are calibrated on the numbers
	 * they have always drawn: pass over as many as it drew from the shared
	 * generator before it had its own - 107 a word, for 693 words, for each
	 * of two kernels.
	 */
	for (draws = 0; draws < 2 * 107 * 693; draws++)
		(void) rnd64();
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

/* ---------- partitions: the nearest centroid, k-means ---------- */

/* k centroids' codes, centred on mean, each array exactly as long as it must be */
typedef struct CentCodes
{
	LionVecCentroids c;
	uint64	   *bits;
	uint16	   *pop;
	float	   *norm;
	float	   *alpha1;
} CentCodes;

static void
cent_codes(CentCodes *cc, const float *cent, int k, int D, const float *mean)
{
	float	   *x = falloc(D);
	uint8	   *codes = (uint8 *) xmalloc(D);
	int			j;

	cc->bits = (uint64 *) xmalloc(sizeof(uint64) * (Size) k * (D / 64));
	cc->pop = (uint16 *) xmalloc(sizeof(uint16) * k);
	cc->norm = falloc(k);
	cc->alpha1 = falloc(k);
	for (j = 0; j < k; j++)
	{
		LionVecEncoded e;

		memcpy(x, cent + (Size) j * D, sizeof(float) * D);
		e.bits = cc->bits + (Size) j * (D / 64);
		e.codes = codes;
		lionvec_encode(mean, D, x, &e);
		cc->pop[j] = e.popcount;
		cc->norm[j] = e.norm;
		cc->alpha1[j] = e.alpha1;
	}
	cc->c.k = k;
	cc->c.dimsp = D;
	cc->c.bits = cc->bits;
	cc->c.popcount = cc->pop;
	cc->c.norm = cc->norm;
	cc->c.alpha1 = cc->alpha1;
	free(codes);
	free(x);
}

static void
cent_codes_free(CentCodes *cc)
{
	free(cc->bits);
	free(cc->pop);
	free(cc->norm);
	free(cc->alpha1);
}

/* the centroid nearest x by L2, ties to the lowest index */
static int
nearest_exact(const float *x, const float *cent, int k, int D, double *dist)
{
	int			best = 0;
	double		bestd = INFINITY;
	int			j;

	for (j = 0; j < k; j++)
	{
		double		d = 0.0;
		int			i;

		for (i = 0; i < D; i++)
		{
			double		t = (double) x[i] - (double) cent[(Size) j * D + i];

			d += t * t;
		}
		if (d < bestd)
		{
			bestd = d;
			best = j;
		}
	}
	if (dist != NULL)
		*dist = bestd;
	return best;
}

static void
test_candidates(void)
{
	static const int dims[] = {64, 320, 1536};
	int			di;

	cur_phase = "candidates";
	for (di = 0; di < 3; di++)
	{
		int			D = dims[di];
		int			k = 200;
		int			T = 20;
		float	   *centers = falloc(T * D);
		float	   *cent = falloc(k * D);
		float	   *mean = falloc(D);
		float	   *x = falloc(D);
		double	   *lo = (double *) xmalloc(sizeof(double) * k);
		double	   *hi = (double *) xmalloc(sizeof(double) * k);
		int		   *cand = (int *) xmalloc(sizeof(int) * k);
		LionVecQuery q;
		CentCodes	cc;
		long		inside = 0;
		long		outside = 0;
		long		found = 0;
		long		trials = 0;
		long		ncand = 0;
		int			trial;
		int			i;
		int			j;

		query_alloc(&q, D);
		/* clustered centroids around T centres, and a mean off the origin */
		for (i = 0; i < T * D; i++)
			centers[i] = (float) rnd_gauss();
		for (i = 0; i < D; i++)
			mean[i] = (float) (rnd_gauss() * 0.3);
		for (j = 0; j < k; j++)
			for (i = 0; i < D; i++)
				cent[(Size) j * D + i] = centers[(Size) (j % T) * D + i] +
					(float) (rnd_gauss() * 0.4);
		cent_codes(&cc, cent, k, D, mean);

		for (trial = 0; trial < 300; trial++)
		{
			int			c = (int) (rnd64() % k);
			int			nc;
			int			best;
			bool		in = false;

			for (i = 0; i < D; i++)
				x[i] = cent[(Size) c * D + i] + (float) (rnd_gauss() * 0.4);
			best = nearest_exact(x, cent, k, D, NULL);

			/* wide bounds: the nearest is always a candidate, and every key within its bounds */
			lionvec_query_init(&q, LIONVEC_L2, mean, x, 10.0);
			nc = lionvec_candidates(&cc.c, &q, lo, hi, cand);
			CHECK(nc >= 1 && nc <= k, "no candidate, or too many");
			for (j = 0; j < nc; j++)
			{
				in |= cand[j] == best;
				if (j > 0)
					CHECK(cand[j] > cand[j - 1], "candidates out of order");
			}
			CHECK(in, "the nearest centroid is not a candidate at 10 standard deviations");
			for (j = 0; j < k; j++)
			{
				/* the key: |c - m|^2 - 2 <c - m, x - m> */
				double		cm2 = 0.0;
				double		ip = 0.0;
				double		key;

				for (i = 0; i < D; i++)
				{
					double		cmi = (double) cent[(Size) j * D + i] - mean[i];

					cm2 += cmi * cmi;
					ip += cmi * ((double) x[i] - mean[i]);
				}
				key = cm2 - 2.0 * ip;
				if (lo[j] - 1e-4 * (1 + fabs(key)) <= key && key <= hi[j] + 1e-4 * (1 + fabs(key)))
					inside++;
				else
					outside++;
			}

			/* the build's bounds: the nearest is nearly always a candidate */
			lionvec_query_init(&q, LIONVEC_L2, mean, x, LIONVEC_ASSIGN_EPS);
			nc = lionvec_candidates(&cc.c, &q, lo, hi, cand);
			ncand += nc;
			trials++;
			for (j = 0; j < nc; j++)
				found += cand[j] == best;
		}
		CHECK(outside == 0, "a centroid's key outside its bounds at 10 standard deviations");
		CHECK(found >= trials - 1, "the nearest centroid missed at the build's bounds");
		/*
		 * and the bounds rule out most centroids, the point of them: all but
		 * the query's own group of k / T at enough dimensions
		 */
		CHECK(ncand < trials * (D >= 320 ? 2 * (k / T) : k / 2),
			  "the candidates are most of the centroids");
		printf("vec_test: dims %4d: %.1f candidates of %d centroids\n", D, (double) ncand / trials, k);
		(void) inside;

		/* no centroids: no candidates */
		cc.c.k = 0;
		CHECK(lionvec_candidates(&cc.c, &q, lo, hi, cand) == 0, "candidates of no centroids");
		cent_codes_free(&cc);

		/* identical centroids: all are candidates */
		for (j = 1; j < 4; j++)
			memcpy(cent + (Size) j * D, cent, sizeof(float) * D);
		cent_codes(&cc, cent, 4, D, mean);
		for (i = 0; i < D; i++)
			x[i] = cent[i] + (float) (rnd_gauss() * 0.4);
		lionvec_query_init(&q, LIONVEC_L2, mean, x, LIONVEC_ASSIGN_EPS);
		CHECK(lionvec_candidates(&cc.c, &q, lo, hi, cand) == 4,
			  "identical centroids are not all candidates");
		cent_codes_free(&cc);

		query_free(&q);
		free(cand);
		free(hi);
		free(lo);
		free(x);
		free(mean);
		free(cent);
		free(centers);
	}
}

/* A sample of n rows as lionvec_kmeans() takes it: 8-bit codes, not centred. */
typedef struct Sample
{
	LionVecSample s;
	uint8	   *codes;
	float	   *scale;
} Sample;

static void
sample_make(Sample *sm, const float *rows, int n, int D)
{
	float	   *x = falloc(D);
	uint64	   *bits = (uint64 *) xmalloc(sizeof(uint64) * (D / 64));
	int			i;

	sm->codes = (uint8 *) xmalloc((Size) n * D);
	sm->scale = falloc(n);
	for (i = 0; i < n; i++)
	{
		LionVecEncoded e;

		memcpy(x, rows + (Size) i * D, sizeof(float) * D);
		e.bits = bits;
		e.codes = sm->codes + (Size) i * D;
		lionvec_encode(NULL, D, x, &e);
		sm->scale[i] = e.rnorm8 > 0.0f ? e.norm / e.rnorm8 : 0.0f;
	}
	sm->s.n = n;
	sm->s.dimsp = D;
	sm->s.codes = sm->codes;
	sm->s.scale = sm->scale;
	free(bits);
	free(x);
}

static void
sample_free(Sample *sm)
{
	free(sm->codes);
	free(sm->scale);
}

static void
kmeans_work(LionVecKmeansWork *w, int n, int k, int D)
{
	w->count = (int *) xmalloc(sizeof(int) * k);
	w->assign = (int *) xmalloc(sizeof(int) * n);
	w->bits = (uint64 *) xmalloc(sizeof(uint64) * (Size) k * (D / 64));
	w->popcount = (uint16 *) xmalloc(sizeof(uint16) * k);
	w->norm = falloc(k);
	w->alpha1 = falloc(k);
	w->lo = (double *) xmalloc(sizeof(double) * k);
	w->hi = (double *) xmalloc(sizeof(double) * k);
	w->cand = (int *) xmalloc(sizeof(int) * k);
	w->x = falloc(D);
	w->y = falloc(D);
	w->codes = (uint8 *) xmalloc(D);
	w->qc = falloc(D);
	w->planes = (uint64 *) xmalloc(sizeof(uint64) * (D / 64) * LIONVEC_QBITS);
}

static void
kmeans_work_free(LionVecKmeansWork *w)
{
	free(w->count);
	free(w->assign);
	free(w->bits);
	free(w->popcount);
	free(w->norm);
	free(w->alpha1);
	free(w->lo);
	free(w->hi);
	free(w->cand);
	free(w->x);
	free(w->y);
	free(w->codes);
	free(w->qc);
	free(w->planes);
}

/* one k-means run, checked for what holds of any: returns the iterations */
static int
kmeans_run(const Sample *sm, int k, int iters, bool unit, const float *mean,
		   float *cent, LionVecKmeansWork *w)
{
	int			D = sm->s.dimsp;
	int			made = lionvec_kmeans(&sm->s, k, iters, unit, 42, mean, cent, w);
	bool		finite = true;
	int			j;
	int			i;

	CHECK(made >= 1 && made <= iters, "iterations made");
	for (i = 0; i < k * D; i++)
		finite &= isfinite(cent[i]);
	CHECK(finite, "a centroid is not finite");
	for (i = 0; i < sm->s.n; i++)
		CHECK(w->assign[i] >= 0 && w->assign[i] < k, "a row assigned to no centroid");
	if (unit)
	{
		bool		ok = true;

		for (j = 0; j < k; j++)
		{
			double		n2 = dot_ff(cent + (Size) j * D, cent + (Size) j * D, D);

			ok &= fabs(n2 - 1.0) < 1e-4;
		}
		CHECK(ok, "a spherical centroid is not of unit length");
	}
	return made;
}

static void
test_kmeans(void)
{
	int			D = 128;
	int			T = 8;
	int			per = 100;
	int			n = T * per;
	float	   *centers = falloc(T * D);
	float	   *rows = falloc(n * D);
	float	   *dec = falloc(n * D);
	float	   *mean = falloc(D);
	float	   *cent = falloc(n * D);
	float	   *cent2 = falloc(n * D);
	LionVecKmeansWork w;
	Sample		sm;
	int			unit;
	int			i;
	int			j;
	int			t;

	cur_phase = "kmeans";
	for (i = 0; i < T * D; i++)
		centers[i] = (float) (rnd_gauss() * 2.0);

	for (unit = 0; unit <= 1; unit++)
	{
		int			made;
		bool		ok;

		/* T well-separated clusters, the rows shuffled among them */
		for (i = 0; i < n; i++)
		{
			int			t = i * 7 % T;

			for (j = 0; j < D; j++)
				rows[(Size) i * D + j] = centers[(Size) t * D + j] + (float) (rnd_gauss() * 0.3);
			if (unit)
			{
				double		n2 = dot_ff(rows + (Size) i * D, rows + (Size) i * D, D);

				for (j = 0; j < D; j++)
					rows[(Size) i * D + j] = (float) (rows[(Size) i * D + j] / sqrt(n2));
			}
		}
		sample_make(&sm, rows, n, D);
		for (i = 0; i < n; i++)
			lionvec_sample_row(&sm.s, i, dec + (Size) i * D);
		{
			/* the codes are within a percent of the rows */
			double		e2 = 0.0;
			double		r2 = 0.0;

			for (i = 0; i < n * D; i++)
			{
				e2 += ((double) dec[i] - rows[i]) * ((double) dec[i] - rows[i]);
				r2 += (double) rows[i] * rows[i];
			}
			CHECK(sqrt(e2 / r2) < 0.01, "a sample's rows are not their codes'");
		}
		for (j = 0; j < D; j++)
		{
			double		s = 0.0;

			for (i = 0; i < n; i++)
				s += dec[(Size) i * D + j];
			mean[j] = (float) (s / n);
		}

		kmeans_work(&w, n, T, D);
		made = kmeans_run(&sm, T, 50, unit, mean, cent, &w);
		CHECK(made < 50, "k-means did not converge on separated clusters");

		/* converged: each row is with the centroid nearest it */
		ok = true;
		for (i = 0; i < n; i++)
			ok &= nearest_exact(dec + (Size) i * D, cent, T, D, NULL) == w.assign[i];
		CHECK(ok, "a row is not with its nearest centroid");

		/* ... and each centroid is its rows' mean (spherical: scaled to unit length) */
		ok = true;
		for (j = 0; j < T; j++)
		{
			double		dist = 0.0;
			double		n2 = 0.0;
			int			c = 0;

			memset(w.x, 0, sizeof(float) * D);
			for (i = 0; i < n; i++)
			{
				if (w.assign[i] != j)
					continue;
				c++;
				for (t = 0; t < D; t++)
					w.x[t] += dec[(Size) i * D + t];
			}
			for (t = 0; t < D; t++)
			{
				w.x[t] /= Max(c, 1);
				n2 += (double) w.x[t] * w.x[t];
			}
			for (t = 0; t < D; t++)
			{
				double		m = unit ? w.x[t] / sqrt(n2) : w.x[t];

				dist += (m - cent[(Size) j * D + t]) * (m - cent[(Size) j * D + t]);
			}
			ok &= c > 0 && sqrt(dist) < 1e-4;
		}
		CHECK(ok, "a centroid is not its rows' mean");

		/* the same seed, the same centroids */
		memcpy(cent2, cent, sizeof(float) * T * D);
		kmeans_run(&sm, T, 50, unit, mean, cent, &w);
		CHECK(memcmp(cent, cent2, sizeof(float) * T * D) == 0, "k-means is not repeatable");

		/* no mean: the same answer, the bounds only looser */
		kmeans_run(&sm, T, 50, unit, NULL, cent2, &w);
		ok = true;
		for (i = 0; i < T * D; i++)
			ok &= fabs(cent[i] - cent2[i]) < 1e-4;
		CHECK(ok, "the mean changed k-means' answer");
		kmeans_work_free(&w);

		/* one centroid: the mean */
		kmeans_work(&w, n, 1, D);
		kmeans_run(&sm, 1, 5, false, mean, cent, &w);
		ok = true;
		for (j = 0; j < D; j++)
			ok &= fabs(cent[j] - mean[j]) < 1e-4;
		CHECK(ok, "one centroid is not the mean");
		kmeans_work_free(&w);

		/* as many centroids as rows: one each */
		kmeans_work(&w, 40, 40, D);
		sm.s.n = 40;
		made = kmeans_run(&sm, 40, 10, unit, mean, cent, &w);
		ok = true;
		for (j = 0; j < 40; j++)
		{
			int			c = 0;

			for (i = 0; i < 40; i++)
				c += w.assign[i] == j;
			ok &= c == 1;
		}
		CHECK(ok, "as many centroids as rows, and not one row each");
		sm.s.n = n;
		kmeans_work_free(&w);

		/*
		 * Eight centroids a cluster: from rows picked at random, as k-means
		 * starts, every cluster gets one (all but certainly), and then no
		 * centroid ends with rows of two clusters.  (With as many centroids
		 * as clusters, two starting rows often share one, which leaves two
		 * clusters to a centroid: random starts are a local search.)
		 */
		kmeans_work(&w, n, 8 * T, D);
		made = kmeans_run(&sm, 8 * T, 50, unit, mean, cent, &w);
		ok = true;
		for (j = 0; j < 8 * T; j++)
		{
			int			first = -1;

			for (i = 0; i < n; i++)
				if (w.assign[i] == j)
				{
					if (first < 0)
						first = i * 7 % T;
					ok &= i * 7 % T == first;
				}
		}
		CHECK(ok, "a centroid has rows of two separated clusters");
		kmeans_work_free(&w);
		sample_free(&sm);

		/* every row the same: k-means ends, and every row is with a centroid */
		for (i = 0; i < 30; i++)
			memcpy(rows + (Size) i * D, rows, sizeof(float) * D);
		sample_make(&sm, rows, 30, D);
		kmeans_work(&w, 30, 3, D);
		kmeans_run(&sm, 3, 10, unit, NULL, cent, &w);
		kmeans_work_free(&w);
		sample_free(&sm);
	}

	free(cent2);
	free(cent);
	free(mean);
	free(dec);
	free(rows);
	free(centers);
}

/*
 * Each set of kernels, end to end: the same query planes, estimates,
 * candidates and k-means as the portable set's, to the bit, on clustered
 * rows - what makes a scan or a build decide alike on any CPU.
 */
static void
test_kernels_agree(void)
{
	int			D = 320;
	int			T = 6;
	int			n = 600;
	int			k = 24;
	float	   *rows = falloc(n * D);
	float	   *centers = falloc(T * D);
	float	   *mean = falloc(D);
	float	   *cent0 = falloc(k * D);
	float	   *cent = falloc(k * D);
	int		   *assign0 = (int *) xmalloc(sizeof(int) * n);
	double	   *est0 = (double *) xmalloc(sizeof(double) * 4 * n);
	double	   *est = (double *) xmalloc(sizeof(double) * 4 * n);
	float	   *x = falloc(D);
	float	   *xc = falloc(D);
	float	   *rot0 = falloc(n * D);
	LionVecRotation rot;
	LionVecKmeansWork w;
	Sample		sm;
	EncBufs		b;
	LionVecQuery q;
	int			which;
	int			i;
	int			j;

	cur_phase = "kernels agree";
	swap_rng();
	for (i = 0; i < T * D; i++)
		centers[i] = (float) rnd_gauss();
	for (i = 0; i < n; i++)
		for (j = 0; j < D; j++)
			rows[(Size) i * D + j] = centers[(Size) (i % T) * D + j] + (float) (0.4 * rnd_gauss());
	for (j = 0; j < D; j++)
	{
		double		s = 0.0;

		for (i = 0; i < n; i++)
			s += rows[(Size) i * D + j];
		mean[j] = (float) (s / n);
	}
	make_rotation(&rot, D, 99);
	enc_alloc(&b, D);
	query_alloc(&q, D);
	sample_make(&sm, rows, n, D);
	kmeans_work(&w, n, k, D);

	for (which = 0; which <= LIONVEC_KERNELS_MAX; which++)
	{
		bool		same = true;

		if (!lionvec_kernel_force(which))
			continue;
		/* rotate, encode against the mean, estimate against row 0's query */
		lionvec_prepare(&rot, LIONVEC_COSINE, rows, D, x);
		lionvec_query_init(&q, LIONVEC_COSINE, mean, x, 3.0);
		for (i = 0; i < n; i++)
		{
			float	   *xi = which == 0 ? rot0 + (Size) i * D : x;

			lionvec_prepare(&rot, LIONVEC_COSINE, rows + (Size) i * D, D, xi);
			if (which > 0)
				same &= memcmp(xi, rot0 + (Size) i * D, sizeof(float) * D) == 0;
			memcpy(xc, xi, sizeof(float) * D);
			lionvec_encode(mean, D, xc, &b.e);
			lionvec_estimate1(&q, b.e.bits, b.e.popcount, b.e.norm, b.e.alpha1,
							  &est[4 * i], &est[4 * i + 1]);
			lionvec_estimate8(&q, b.e.codes, b.e.norm, b.e.alpha8, b.e.rnorm8,
							  &est[4 * i + 2], &est[4 * i + 3]);
		}
		lionvec_kmeans(&sm.s, k, 10, true, 7, mean, cent, &w);
		if (which == 0)
		{
			memcpy(est0, est, sizeof(double) * 4 * n);
			memcpy(cent0, cent, sizeof(float) * k * D);
			memcpy(assign0, w.assign, sizeof(int) * n);
			continue;
		}
		CHECK(same, "a kernel set rotates other than the portable one");
		CHECK(memcmp(est, est0, sizeof(double) * 4 * n) == 0,
			  "a kernel set estimates other than the portable one");
		CHECK(memcmp(cent, cent0, sizeof(float) * k * D) == 0 &&
			  memcmp(w.assign, assign0, sizeof(int) * n) == 0,
			  "a kernel set's k-means differs from the portable one's");
	}
	lionvec_kernels_init();
	swap_rng();

	kmeans_work_free(&w);
	sample_free(&sm);
	query_free(&q);
	enc_free(&b);
	free(rot.signs);
	free(rot0);
	free(xc);
	free(x);
	free(est);
	free(est0);
	free(assign0);
	free(cent);
	free(cent0);
	free(mean);
	free(centers);
	free(rows);
}

/* ---------- spill: ranking, the m nearest's candidates, SOAR ---------- */

/* NaN as infinity, as lionvec_rank() takes it */
static double
rank_key(double k)
{
	return isnan(k) ? INFINITY : k;
}

/*
 * lionvec_rank() against a reference - every place, the m least by a
 * stable selection - for keys with ties, NaNs and infinities, of every
 * length up to 40 and every m up to LIONVEC_RANK_MAX.
 */
static void
test_rank(void)
{
	double		key[40];
	int			order[LIONVEC_RANK_MAX];
	int			ref[40];
	bool		taken[40];
	bool		ok = true;
	int			trial;

	cur_phase = "rank";
	for (trial = 0; trial < 400; trial++)
	{
		int			n = (int) (rnd64() % 41);
		int			m = (int) (rnd64() % (LIONVEC_RANK_MAX + 1));
		int			r;
		int			want = Min(n, m);
		int			i;
		int			j;

		for (i = 0; i < n; i++)
		{
			uint64		u = rnd64() % 20;

			/* few distinct values, so that ties are common */
			key[i] = u == 0 ? NAN : u == 1 ? INFINITY : u == 2 ? -INFINITY :
				(double) (rnd64() % 6);
		}
		/* the reference: the least not yet taken, the lowest place among equals */
		memset(taken, 0, sizeof(taken));
		for (j = 0; j < want; j++)
		{
			int			b = -1;

			for (i = 0; i < n; i++)
				if (!taken[i] && (b < 0 || rank_key(key[i]) < rank_key(key[b])))
					b = i;
			taken[b] = true;
			ref[j] = b;
		}
		r = lionvec_rank(key, n, m, order);
		ok &= r == want;
		for (j = 0; j < want && r == want; j++)
			ok &= order[j] == ref[j];
	}
	CHECK(ok, "rank: not the m least, least first, ties to the lower place");
	CHECK(lionvec_rank(key, 0, 4, order) == 0, "rank of no keys");
	CHECK(lionvec_rank(key, 5, 0, order) == 0, "rank of none wanted");
}

/*
 * lionvec_candidates_n() on clustered centroids: m = 1 is
 * lionvec_candidates(); the exact m nearest are always candidates at a wide
 * bound and nearly always at the build's; there are at least m; fewer
 * centroids than m are all candidates.
 */
static void
test_candidates_n(void)
{
	static const int dims[] = {64, 320, 1536};
	int			di;

	cur_phase = "candidates_n";
	for (di = 0; di < 3; di++)
	{
		int			D = dims[di];
		int			k = 200;
		int			T = 20;
		int			m = LIONVEC_SOAR_CANDS + 1;
		float	   *centers = falloc(T * D);
		float	   *cent = falloc(k * D);
		float	   *x = falloc(D);
		double	   *lo = (double *) xmalloc(sizeof(double) * k);
		double	   *hi = (double *) xmalloc(sizeof(double) * k);
		double	   *lo1 = (double *) xmalloc(sizeof(double) * k);
		double	   *hi1 = (double *) xmalloc(sizeof(double) * k);
		int		   *cand = (int *) xmalloc(sizeof(int) * k);
		int		   *cand1 = (int *) xmalloc(sizeof(int) * k);
		double	   *dist = (double *) xmalloc(sizeof(double) * k);
		int			near[LIONVEC_SOAR_CANDS + 1];
		LionVecQuery q;
		CentCodes	cc;
		long		missed_wide = 0;
		long		missed = 0;
		long		ncand = 0;
		bool		same = true;
		bool		enough = true;
		int			trial;
		int			i;
		int			j;

		query_alloc(&q, D);
		for (i = 0; i < T * D; i++)
			centers[i] = (float) rnd_gauss();
		for (j = 0; j < k; j++)
			for (i = 0; i < D; i++)
				cent[(Size) j * D + i] = centers[(Size) (j % T) * D + i] +
					(float) (rnd_gauss() * 0.4);
		cent_codes(&cc, cent, k, D, NULL);

		for (trial = 0; trial < 200; trial++)
		{
			int			c = (int) (rnd64() % k);
			int			nc;
			int			nc1;

			for (i = 0; i < D; i++)
				x[i] = cent[(Size) c * D + i] + (float) (rnd_gauss() * 0.4);
			for (j = 0; j < k; j++)
			{
				dist[j] = 0.0;
				for (i = 0; i < D; i++)
				{
					double		t = (double) x[i] - cent[(Size) j * D + i];

					dist[j] += t * t;
				}
			}
			CHECK(lionvec_rank(dist, k, m, near) == m, "rank of the distances");

			lionvec_query_init(&q, LIONVEC_L2, NULL, x, 10.0);
			nc = lionvec_candidates_n(&cc.c, &q, m, lo, hi, cand);
			enough &= nc >= m;
			for (j = 0; j < m; j++)
			{
				bool		in = false;
				int			c2;

				for (c2 = 0; c2 < nc; c2++)
					in |= cand[c2] == near[j];
				missed_wide += !in;
			}

			lionvec_query_init(&q, LIONVEC_L2, NULL, x, LIONVEC_ASSIGN_EPS);
			nc = lionvec_candidates_n(&cc.c, &q, m, lo, hi, cand);
			ncand += nc;
			enough &= nc >= m;
			for (j = 0; j < m; j++)
			{
				bool		in = false;
				int			c2;

				for (c2 = 0; c2 < nc; c2++)
					in |= cand[c2] == near[j];
				missed += !in;
			}
			nc = lionvec_candidates_n(&cc.c, &q, 1, lo, hi, cand);
			nc1 = lionvec_candidates(&cc.c, &q, lo1, hi1, cand1);
			same &= nc == nc1 && memcmp(cand, cand1, sizeof(int) * nc) == 0;
		}
		CHECK(same, "candidates_n of 1 is not candidates");
		CHECK(enough, "fewer candidates than m");
		CHECK(missed_wide == 0, "one of the m nearest is not a candidate at 10 sd");
		/* at most one of the 1,800 pairs, as the nearest alone misses one in 200 */
		CHECK(missed <= 1, "one of the m nearest missed at the build's bounds");
		/*
		 * and few: all but the row's own group of k / T at enough dimensions,
		 * as for the nearest alone (test_candidates())
		 */
		CHECK(ncand < 200L * (D >= 320 ? 2 * (k / T) : k * 3 / 4),
			  "the candidates of the m nearest are most of the centroids");
		printf("vec_test: dims %4d: %.1f candidates for the %d nearest of %d centroids\n",
			   D, (double) ncand / 200, m, k);

		/* fewer centroids than m: all of them */
		cc.c.k = 5;
		CHECK(lionvec_candidates_n(&cc.c, &q, m, lo, hi, cand) == 5,
			  "fewer centroids than m are not all candidates");
		cc.c.k = 0;
		CHECK(lionvec_candidates_n(&cc.c, &q, m, lo, hi, cand) == 0,
			  "candidates of no centroids");
		cent_codes_free(&cc);

		query_free(&q);
		free(dist);
		free(cand1);
		free(cand);
		free(hi1);
		free(lo1);
		free(hi);
		free(lo);
		free(x);
		free(cent);
		free(centers);
	}
}

/*
 * lionvec_decode8(): the codes' vector scaled to x's projection on it, so
 * |x - y|^2 = |x|^2 (1 - alpha8^2), about a percent of |x| at any dimension
 * (the 8-bit steps' rounding, and a clipped coordinate now and then); and
 * no length, no vector.
 */
static void
test_decode8(void)
{
	static const int dims[] = {64, 320, 1536, 8128};
	int			di;

	cur_phase = "decode8";
	for (di = 0; di < (int) lengthof(dims); di++)
	{
		int			D = dims[di];
		float	   *x = falloc(D);
		float	   *xc = falloc(D);
		float	   *y = falloc(D);
		float	   *mean = falloc(D);
		EncBufs		b;
		bool		exact = true;
		bool		near = true;
		bool		proj = true;
		double		rel = 0.0;
		int			trial;
		int			i;

		enc_alloc(&b, D);
		for (i = 0; i < D; i++)
			mean[i] = (float) (rnd_gauss() * 0.3);
		for (trial = 0; trial < 40; trial++)
		{
			double		n2;
			double		e2 = 0.0;
			double		ye = 0.0;

			for (i = 0; i < D; i++)
				x[i] = mean[i] + (float) (rnd_gauss() * (0.1 + trial % 4));
			lionvec_encode(mean, D, x, &b.e);	/* centres x */
			memcpy(xc, x, sizeof(float) * D);
			lionvec_decode8(b.codes, D, b.e.norm, b.e.alpha8, b.e.rnorm8, y);
			n2 = dot_ff(xc, xc, D);
			for (i = 0; i < D; i++)
			{
				double		e = (double) xc[i] - y[i];

				e2 += e * e;
				ye += e * y[i];
			}
			/* float rounding of each coordinate is far below the codes' error */
			exact &= fabs(e2 - n2 * (1.0 - (double) b.e.alpha8 * b.e.alpha8)) <=
				1e-3 * n2 * (1.0 - (double) b.e.alpha8 * b.e.alpha8) + 1e-9 * n2;
			/* a 32nd of a standard deviation a coordinate, and the rare clipped one */
			near &= e2 <= 0.03 * 0.03 * n2;
			rel += sqrt(e2 / n2);
			/* the error is orthogonal to y: y is x's projection */
			proj &= fabs(ye) <= 1e-4 * n2;
		}
		CHECK(exact, "decode8 is not |x|^2 (1 - alpha8^2) from x");
		CHECK(near, "decode8 is more than 3% of |x| from x");
		/* (1/32) / sqrt(12) = 0.9% of |x| for uniform rounding */
		CHECK(rel / 40 < 0.012, "decode8 is on average more than 1.2% of |x| from x");
		CHECK(proj, "decode8 is not x's projection on the codes' vector");

		/* a row at the mean: codes with no length */
		memcpy(x, mean, sizeof(float) * D);
		lionvec_encode(mean, D, x, &b.e);
		lionvec_decode8(b.codes, D, b.e.norm, b.e.alpha8, b.e.rnorm8, y);
		{
			bool		zero = true;

			for (i = 0; i < D; i++)
				zero &= y[i] == 0.0f;
			CHECK(zero, "decode8 of a vector at the mean is not zero");
		}
		lionvec_decode8(b.codes, D, 1.0f, 1.0f, 0.0f, y);
		{
			bool		zero = true;

			for (i = 0; i < D; i++)
				zero &= y[i] == 0.0f;
			CHECK(zero, "decode8 of codes of no length is not zero");
		}
		enc_free(&b);
		free(mean);
		free(y);
		free(xc);
		free(x);
	}
}

/* SOAR's cost of c, the obvious way: rhat as a vector, then the projection */
static double
soar_cost_ref(const float *x, const float *c1, const float *c, int D, double lambda)
{
	double	   *rhat = (double *) xmalloc(sizeof(double) * D);
	double		rn = 0.0;
	double		d2 = 0.0;
	double		p = 0.0;
	int			i;

	for (i = 0; i < D; i++)
	{
		rhat[i] = (double) x[i] - c1[i];
		rn += rhat[i] * rhat[i];
	}
	rn = sqrt(rn);
	for (i = 0; i < D; i++)
	{
		double		d = (double) x[i] - c[i];

		if (rn > 0.0)
			p += rhat[i] / rn * d;
		d2 += d * d;
	}
	free(rhat);
	return d2 + lambda * p * p;
}

/*
 * SOAR's choice on vectors whose answer is known: x at the origin, c1 at
 * -e0, so that the residual points along e0; a candidate at e0, 1 away and
 * straight along it, costs 1 + lambda; one at t e1, t away and square to
 * it, t^2.  So with lambda = 1 the square one wins below t = sqrt(2) and
 * loses above it, and with lambda = 0 the nearer wins.  Then the cost's
 * algebra against soar_cost_ref() on random vectors; one candidate, none,
 * identical ones (the first), a row at c1 (no residual: the nearest), and
 * a candidate that is not a number.
 */
static void
test_soar_algebra(void)
{
	int			D = 64;
	float	   *x = falloc(D);
	float	   *c1 = falloc(D);
	float	   *a = falloc(D);
	float	   *b = falloc(D);
	float	   *c = falloc(D);
	const float *cs[4];
	bool		ok = true;
	int			trial;
	int			i;

	cur_phase = "soar";
	memset(x, 0, sizeof(float) * D);
	memset(c1, 0, sizeof(float) * D);
	memset(a, 0, sizeof(float) * D);
	memset(b, 0, sizeof(float) * D);
	c1[0] = -1.0f;
	a[0] = 1.0f;

	b[1] = 1.4f;
	cs[0] = a;
	cs[1] = b;
	CHECK(fabs(lionvec_soar_cost(x, c1, a, D, 1.0) - 2.0) < 1e-12, "soar: along the residual costs 2");
	CHECK(fabs(lionvec_soar_cost(x, c1, b, D, 1.0) - 1.4 * 1.4) < 1e-6, "soar: square to it costs t^2");
	CHECK(fabs(lionvec_soar_cost(x, c1, a, D, 0.5) - 1.5) < 1e-12, "soar: lambda scales the parallel part");
	CHECK(lionvec_soar_second(x, c1, cs, 2, D, 1.0) == 1, "soar: t = 1.4 is not chosen");
	CHECK(lionvec_soar_second(x, c1, cs, 2, D, 0.0) == 0, "soar: lambda 0 is not the nearest");
	b[1] = 1.42f;
	CHECK(lionvec_soar_second(x, c1, cs, 2, D, 1.0) == 0, "soar: t = 1.42 is chosen");
	CHECK(lionvec_soar_second(x, c1, cs, 2, D, LIONVEC_SOAR_LAMBDA) == 0, "soar: the default lambda is not 1");
	b[1] = 1.4f;
	CHECK(lionvec_soar_second(x, c1, cs, 2, D, LIONVEC_SOAR_LAMBDA) == 1, "soar: the default lambda is not 1");

	/* the algebra, against the obvious way */
	for (trial = 0; trial < 200; trial++)
	{
		double		lambda = trial % 3 == 0 ? 1.0 : 3.0 * rnd_unit();
		double		got;
		double		want;

		for (i = 0; i < D; i++)
		{
			x[i] = (float) rnd_gauss();
			c1[i] = (float) (x[i] + rnd_gauss() * 0.3);
			c[i] = (float) (x[i] + rnd_gauss() * 0.5);
		}
		got = lionvec_soar_cost(x, c1, c, D, lambda);
		want = soar_cost_ref(x, c1, c, D, lambda);
		ok &= fabs(got - want) <= 1e-9 * (1.0 + want);
		/* lambda = 1: |d|^2 (1 + cos^2) of the angle between d and r */
		if (lambda == 1.0)
		{
			double		d2 = 0.0;
			double		rd = 0.0;
			double		r2 = 0.0;

			for (i = 0; i < D; i++)
			{
				double		d = (double) x[i] - c[i];
				double		r = (double) x[i] - c1[i];

				d2 += d * d;
				rd += r * d;
				r2 += r * r;
			}
			ok &= fabs(got - d2 * (1.0 + rd * rd / (r2 * d2))) <= 1e-9 * (1.0 + got);
		}
	}
	CHECK(ok, "soar: the cost is not |d|^2 + lambda <rhat, d>^2");

	/* one candidate, none, identical ones */
	cs[0] = c;
	CHECK(lionvec_soar_second(x, c1, cs, 1, D, 1.0) == 0, "soar: one candidate is not chosen");
	CHECK(lionvec_soar_second(x, c1, cs, 0, D, 1.0) == -1, "soar: no candidates");
	cs[0] = cs[1] = cs[2] = cs[3] = c;
	CHECK(lionvec_soar_second(x, c1, cs, 4, D, 1.0) == 0, "soar: identical candidates are not a tie to the first");

	/* a row at c1: no residual, so the nearest */
	memcpy(c1, x, sizeof(float) * D);
	for (i = 0; i < D; i++)
	{
		a[i] = (float) (x[i] + rnd_gauss() * 0.5);
		b[i] = (float) (x[i] + rnd_gauss() * 0.2);
	}
	cs[0] = a;
	cs[1] = b;
	CHECK(lionvec_soar_second(x, c1, cs, 2, D, 1.0) == 1, "soar: a row at c1 does not go to the nearest");
	CHECK(fabs(lionvec_soar_cost(x, c1, a, D, 1.0) - soar_cost_ref(x, c1, a, D, 0.0)) <
		  1e-9 * (1.0 + soar_cost_ref(x, c1, a, D, 0.0)), "soar: a row at c1 costs |d|^2");

	/* not a number: never chosen over a number, and infinite */
	a[3] = NAN;
	cs[0] = a;
	CHECK(isinf(lionvec_soar_cost(x, c1, a, D, 1.0)), "soar: a NaN cost is not infinite");
	CHECK(lionvec_soar_second(x, c1, cs, 2, D, 1.0) == 1, "soar: a NaN candidate is chosen");
	cs[1] = a;
	CHECK(lionvec_soar_second(x, c1, cs, 2, D, 1.0) == 0, "soar: all NaN, not the first");

	free(c);
	free(b);
	free(a);
	free(c1);
	free(x);
}

/*
 * SOAR's choice on clustered rows, as the build makes it: each row's
 * nearest centroid by L2, the next LIONVEC_SOAR_CANDS nearest as the
 * candidates, nearest first.  The choice has the least cost of them all by
 * the reference; it differs from the plain second-nearest in a measurable
 * share of rows, and when it does it lies further off the residual's line;
 * with lambda 0 it is the second-nearest.  Made as an insert makes it, from
 * centroids decoded from their 8-bit codes, it is the same for nearly every
 * row, and otherwise one whose cost is within a percent.
 */
static void
test_soar_clustered(void)
{
	static const int dims[] = {64, 384, 1536};
	int			di;

	cur_phase = "soar clustered";
	for (di = 0; di < (int) lengthof(dims); di++)
	{
		int			D = dims[di];
		int			k = 120;
		int			T = 12;
		int			n = 600;
		int			m = LIONVEC_SOAR_CANDS + 1;
		float	   *centers = falloc(T * D);
		float	   *cent = falloc(k * D);
		float	   *dec = falloc(k * D);
		float	   *x = falloc(D);
		double	   *dist = (double *) xmalloc(sizeof(double) * k);
		int			near[LIONVEC_SOAR_CANDS + 1];
		const float *cs[LIONVEC_SOAR_CANDS];
		const float *ds[LIONVEC_SOAR_CANDS];
		EncBufs		b;
		long		differs = 0;
		long		agree = 0;
		long		close = 0;
		double		cos_soar = 0.0;
		double		cos_plain = 0.0;
		bool		least = true;
		bool		plain = true;
		int			row;
		int			i;
		int			j;

		enc_alloc(&b, D);
		for (i = 0; i < T * D; i++)
			centers[i] = (float) rnd_gauss();
		for (j = 0; j < k; j++)
			for (i = 0; i < D; i++)
				cent[(Size) j * D + i] = centers[(Size) (j % T) * D + i] +
					(float) (rnd_gauss() * 0.4);
		/* the centroids as an insert sees them */
		for (j = 0; j < k; j++)
		{
			memcpy(x, cent + (Size) j * D, sizeof(float) * D);
			lionvec_encode(NULL, D, x, &b.e);
			lionvec_decode8(b.codes, D, b.e.norm, b.e.alpha8, b.e.rnorm8, dec + (Size) j * D);
		}

		for (row = 0; row < n; row++)
		{
			int			c = (int) (rnd64() % k);
			const float *c1;
			int			s;
			int			sd;
			double		best;

			for (i = 0; i < D; i++)
				x[i] = cent[(Size) c * D + i] + (float) (rnd_gauss() * 0.4);
			for (j = 0; j < k; j++)
			{
				dist[j] = 0.0;
				for (i = 0; i < D; i++)
				{
					double		t = (double) x[i] - cent[(Size) j * D + i];

					dist[j] += t * t;
				}
			}
			(void) lionvec_rank(dist, k, m, near);
			c1 = cent + (Size) near[0] * D;
			for (j = 1; j < m; j++)
			{
				cs[j - 1] = cent + (Size) near[j] * D;
				ds[j - 1] = dec + (Size) near[j] * D;
			}
			s = lionvec_soar_second(x, c1, cs, m - 1, D, 1.0);
			plain &= lionvec_soar_second(x, c1, cs, m - 1, D, 0.0) == 0;
			best = soar_cost_ref(x, c1, cs[s], D, 1.0);
			for (j = 0; j < m - 1; j++)
				least &= best <= soar_cost_ref(x, c1, cs[j], D, 1.0) * (1.0 + 1e-9);
			if (s != 0)
			{
				double		r2 = 0.0;
				double		rp = 0.0;
				double		rs = 0.0;
				double		p2 = 0.0;
				double		s2 = 0.0;

				differs++;
				for (i = 0; i < D; i++)
				{
					double		r = (double) x[i] - c1[i];
					double		dp = (double) x[i] - cs[0][i];
					double		ds2 = (double) x[i] - cs[s][i];

					r2 += r * r;
					rp += r * dp;
					rs += r * ds2;
					p2 += dp * dp;
					s2 += ds2 * ds2;
				}
				cos_plain += rp * rp / (r2 * p2);
				cos_soar += rs * rs / (r2 * s2);
			}

			/* as an insert chooses: of decoded centroids */
			sd = lionvec_soar_second(x, dec + (Size) near[0] * D, ds, m - 1, D, 1.0);
			agree += sd == s;
			close += sd == s ||
				soar_cost_ref(x, c1, cs[sd], D, 1.0) <= best * 1.01;
		}
		printf("vec_test: dims %4d: SOAR chose other than the second-nearest for %.1f%% of rows "
			   "(cos^2 to the residual %.3f, against %.3f); decoded centroids agreed on %.1f%%\n",
			   D, 100.0 * differs / n, cos_soar / Max(differs, 1), cos_plain / Max(differs, 1),
			   100.0 * agree / n);
		CHECK(least, "soar: the choice does not have the least cost");
		CHECK(plain, "soar: lambda 0 is not the second-nearest");
		CHECK(differs >= n / 10 && differs < n, "soar: the choice is the second-nearest too often, or never");
		CHECK(cos_soar < cos_plain, "soar: its choices lie no further off the residual than the second-nearest");
		CHECK(agree >= n * 9 / 10, "soar: decoded centroids choose otherwise too often");
		CHECK(close == n, "soar: a decoded choice costs more than a percent above the float one");

		enc_free(&b);
		free(dist);
		free(x);
		free(dec);
		free(cent);
		free(centers);
	}
}

/* ---------- an inner product's partition bound ---------- */

/*
 * lionvec_lift_key() against partitions of rows of mixed lengths, and of
 * unit rows: with a margin of the lifted rows' greatest distance from their
 * lifted centroid it bounds every row's key, for any query, which is the
 * triangle inequality there and so the proof that the lift's algebra - the
 * offset, |q|, the spread - fits; on unit rows it is the L2 bound of the
 * unit query; it falls as the margin grows, is at most the centroid's key
 * from a margin of 0, and passes infinities and a zero query through.
 */
static void
test_lift(void)
{
	int			dims = 24;
	int			n = 300;
	float	   *x = falloc(n * dims);
	double	   *c = (double *) xmalloc(sizeof(double) * dims);
	float	   *q = falloc(dims);
	int			unit;

	cur_phase = "lift";
	for (unit = 0; unit <= 1; unit++)
	{
		int			trial;

		for (trial = 0; trial < 20; trial++)
		{
			double		m2 = 0.0;
			double		c2 = 0.0;
			double		spread = 0.0;
			double		h;
			double		radius = 0.0;
			double		offset = rnd_gauss();
			bool		holds = true;
			bool		matches = true;
			bool		falls = true;
			int			i;
			int			d;
			int			t;

			/* rows about a centre, of lengths from 0.2 to 3 times its */
			for (d = 0; d < dims; d++)
				c[d] = rnd_gauss();
			for (i = 0; i < n; i++)
			{
				double		len = unit ? 0.0 : 0.2 + 2.8 * rnd_unit();
				double		r2 = 0.0;

				for (d = 0; d < dims; d++)
				{
					double		v = (unit ? c[d] : len * c[d]) + 0.4 * rnd_gauss();

					x[i * dims + d] = (float) v;
					r2 += v * v;
				}
				if (unit)
				{
					for (d = 0; d < dims; d++)
						x[i * dims + d] = (float) (x[i * dims + d] / sqrt(r2));
				}
			}
			/* the centroid, the greatest |x|^2, the spread */
			for (d = 0; d < dims; d++)
			{
				c[d] = 0.0;
				for (i = 0; i < n; i++)
					c[d] += x[i * dims + d];
				c[d] /= n;
				c2 += c[d] * c[d];
			}
			for (i = 0; i < n; i++)
			{
				double		r2 = dot_ff(x + i * dims, x + i * dims, dims);
				double		e2 = 0.0;

				m2 = Max(m2, r2);
				for (d = 0; d < dims; d++)
					e2 += (x[i * dims + d] - c[d]) * (x[i * dims + d] - c[d]);
				spread += e2 / n;
			}
			/* the lifted centroid (c, h), and the lifted rows' farthest */
			h = sqrt(Max(m2 - spread - c2, 0.0));
			for (i = 0; i < n; i++)
			{
				double		r2 = dot_ff(x + i * dims, x + i * dims, dims);
				double		e2 = 0.0;
				double		lift = sqrt(Max(m2 - r2, 0.0));

				for (d = 0; d < dims; d++)
					e2 += (x[i * dims + d] - c[d]) * (x[i * dims + d] - c[d]);
				e2 += (lift - h) * (lift - h);
				radius = Max(radius, sqrt(e2));
			}

			for (t = 0; t < 20; t++)
			{
				double		scale = 0.1 + 10.0 * rnd_unit();
				double		qnorm = 0.0;
				double		qcent = 0.0;
				double		lo;
				double		key;
				double		least = INFINITY;

				for (d = 0; d < dims; d++)
				{
					q[d] = (float) (scale * rnd_gauss());
					qnorm += (double) q[d] * q[d];
					qcent += q[d] * c[d];
				}
				qnorm = sqrt(qnorm);
				lo = -qcent + offset;
				key = lionvec_lift_key(lo, offset, qnorm, m2, spread, radius * (1.0 + 1e-9));
				for (i = 0; i < n; i++)
					least = Min(least, -dot_ff(q, x + i * dims, dims) + offset);
				if (key > least + 1e-9 * (1.0 + fabs(least)))
					holds = false;

				if (unit)
				{
					/* the L2 bound of q / |q|: |q / |q| - c| less the margin */
					double		dc = sqrt(Max(1.0 + c2 - 2.0 * qcent / qnorm, 0.0));
					double		dm = Max(dc - 0.1, 0.0);
					double		want = qnorm * (dm * dm - 2.0) / 2.0 + offset;

					if (fabs(lionvec_lift_key(lo, offset, qnorm, m2, spread, 0.1) - want) >
						1e-6 * (1.0 + fabs(want)))
						matches = false;
				}

				if (!(lionvec_lift_key(lo, offset, qnorm, m2, spread, 0.0) <= lo + 1e-12 &&
					  lionvec_lift_key(lo, offset, qnorm, m2, spread, 0.5) <=
					  lionvec_lift_key(lo, offset, qnorm, m2, spread, 0.1)))
					falls = false;
			}
			CHECK(holds, unit ? "lift: unit rows beat the bound at their radius" :
				  "lift: rows of mixed lengths beat the bound at their radius");
			if (unit)
				CHECK(matches, "lift: unit rows not bounded as L2 bounds them");
			CHECK(falls, "lift: the bound does not fall with the margin");
		}
	}
	CHECK(isinf(lionvec_lift_key(-INFINITY, 1.0, 2.0, 1.0, 0.1, 1.0)), "lift: -inf");
	CHECK(isinf(lionvec_lift_key(INFINITY, 1.0, 2.0, 1.0, 0.1, 1.0)), "lift: inf");
	CHECK(lionvec_lift_key(0.25, 1.0, 0.0, 1.0, 0.1, 1.0) == 0.25, "lift: a zero query");
	free(x);
	free(c);
	free(q);
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
	test_lift();
	test_candidates();
	test_kmeans();
	test_kernels_agree();
	test_rank();
	test_candidates_n();
	test_decode8();
	test_soar_algebra();
	test_soar_clustered();
	test_coverage();

	printf("vec_test: %ld checks, %ld failed\n", nchecks, nfail);
	return nfail == 0 ? 0 : 1;
}
