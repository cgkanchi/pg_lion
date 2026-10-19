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
 *	  - every kernel the build and CPU have (portable, POPCNT, AVX2,
 *		AVX-512): plane 1's against a naive count, the sums against their
 *		sixteen lanes added as the header says and the rotation against the
 *		portable one's, to the bit, at every dimsp an index takes and from
 *		unaligned pointers; and end to end, the same rotations, estimates and
 *		k-means as the portable set's;
 *	  - a halfvec's halves: the portable conversion of each of the 65,536
 *		against its value computed in double from its sign, exponent and
 *		mantissa, a NaN a quiet NaN of its payload; every set's (F16C where
 *		the CPU has it) against the portable one's to the bit, NaNs
 *		included, at every length of a tail and every alignment; and the
 *		bounds a vector or halfvec datum is checked against;
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
 *		about a third of the time, at three almost always;
 *	  - prefetch: the items a CODE page holds and the pages a chain of them
 *		takes, the window a reader asks for, and the least keys of a queue's
 *		top by a heap, lionvec_rank()'s set;
 *	  - the locator (vectors in TOAST): the chunks a vector takes, the runs
 *		made of their places and their checks, and the LOC pages a scan's
 *		members choose, against models;
 *	  - the exact path's price of the pages it reads in order: a bitmap heap
 *		scan's, as costsize.c prices it, bounded by the rows and the pages;
 *	  - the partitions a build makes and wants, and the sample they take:
 *		the rule's figures, a sample of every row never short, a short one
 *		short for every larger table, and the maintenance_work_mem a build's
 *		error names enough where a kilobyte less is not.
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
	"portable", "POPCNT", "AVX2", "AVX-512"
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

/* ---------- a halfvec's halves, and pgvector's datums ---------- */

/*
 * The value of the half h, from its fields, in double: what lionvec_halves()
 * must give, stated apart from it.
 */
static double
half_reference(uint16 h)
{
	int			sign = h >> 15;
	int			e = (h >> 10) & 0x1f;
	int			m = h & 0x3ff;
	double		v;

	if (e == 31)
		v = m == 0 ? INFINITY : NAN;
	else if (e == 0)
		v = ldexp((double) m, -24);	/* subnormal: m 2^-10 of 2^-14 */
	else
		v = ldexp(1.0 + (double) m / 1024.0, e - 15);
	return sign ? -v : v;
}


/*
 * Every half converted: the portable kernel against half_reference(), bit
 * for bit but any NaN for a NaN, and its NaNs quiet with their payloads;
 * then every set's kernel against the portable one's, bit for bit, NaNs
 * included - once over all 65,536 in one array, which F16C converts eight
 * at a time, and at every length from 0 to 40 from every alignment of
 * eight, which ends in a tail, into exact-size buffers and no further.
 */
static void
test_halves(void)
{
	uint16	   *h = (uint16 *) xmalloc(sizeof(uint16) * 65536);
	float	   *port = falloc(65536);
	float	   *x = falloc(65536);
	bool		ok = true;
	bool		quiet = true;
	int			nnan = 0;
	int			which;
	int			ran = 0;
	char		names[160] = "";
	int			i;

	cur_phase = "halves";
	for (i = 0; i < 65536; i++)
		h[i] = (uint16) i;

	lionvec_kernel_force(LIONVEC_KERNELS_PORTABLE);
	lionvec_halves(h, 65536, port);
	for (i = 0; i < 65536; i++)
	{
		float		r = (float) half_reference(h[i]);
		uint32		bits;

		memcpy(&bits, &port[i], sizeof(bits));
		if (isnan(r))
		{
			nnan++;
			ok &= isnan(port[i]);
			/* the quiet bit, and the payload, at the top of the mantissa */
			quiet &= bits == (((uint32) (h[i] & 0x8000) << 16) | 0x7fc00000 |
							  ((uint32) (h[i] & 0x3ff) << 13));
		}
		else
			ok &= memcmp(&r, &port[i], sizeof(float)) == 0;
	}
	CHECK(ok, "a half's float is not its value");
	CHECK(nnan == 2 * 1023, "not every NaN half was taken for one");
	CHECK(quiet, "a NaN half's float is not the quiet NaN of its payload");
	CHECK(port[0x0001] == 5.9604644775390625e-8f && port[0x8000] == 0.0f &&
		  signbit(port[0x8000]) && port[0x7bff] == 65504.0f &&
		  port[0x0400] == 6.103515625e-5f && port[0x03ff] < port[0x0400] &&
		  isinf(port[0xfc00]) && port[0xfc00] < 0,
		  "a landmark half's float");

	for (which = 0; which <= LIONVEC_KERNELS_MAX; which++)
	{
		bool		same = true;
		bool		tails = true;
		uint32		k = 0;
		int			off;
		int			n;

		if (!lionvec_kernel_force(which))
			continue;
		ran++;
		snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s%s",
				 ran > 1 ? ", " : "", kernel_names[which],
				 lionvec_kernel_f16c() ? " (F16C)" : "");

		memset(x, 0, sizeof(float) * 65536);
		lionvec_halves(h, 65536, x);
		same &= memcmp(x, port, sizeof(float) * 65536) == 0;

		for (off = 0; off < 8; off++)
		{
			for (n = 0; n <= 40; n++)
			{
				uint16	   *src = (uint16 *) xmalloc(sizeof(uint16) * (off + n));
				float	   *dst = falloc(off + n + 1);
				const float sentinel = 1234.5f;

				for (i = 0; i < n; i++)
					src[off + i] = (uint16) ((++k * 0x9E3779B1u) >> 16);
				dst[off + n] = sentinel;
				lionvec_halves(src + off, n, dst + off);
				for (i = 0; i < n; i++)
					tails &= memcmp(&dst[off + i], &port[src[off + i]], sizeof(float)) == 0;
				tails &= memcmp(&dst[off + n], &sentinel, sizeof(float)) == 0;
				free(src);
				free(dst);
			}
		}
		CHECK(same, "a set converts the halves other than the portable kernel");
		CHECK(tails, "a set converts a short or unaligned run of halves wrong");
	}
	CHECK(ran >= 1, "no kernel ran");
	printf("vec_test: halves converted by the sets: %s\n", names);
	lionvec_kernels_init();

	free(x);
	free(port);
	free(h);
}

/*
 * lionvec_datum_dim(): the bytes a vector or halfvec datum must have - its
 * header, a count no less than zero, and the count's elements - from
 * exact-size buffers, so that `make unit SANITIZE=1` would see the count
 * read from a datum too short to hold it.
 */
static int
datum_dim(Size len, int16 dim, bool half)
{
	char	   *p = (char *) xmalloc(len);
	int			got;

	if (len >= offsetof(LionVecPgVector, x))
	{
		LionVecPgVector hdr;

		memset(&hdr, 0, sizeof(hdr));
		hdr.dim = dim;
		memcpy(p, &hdr, offsetof(LionVecPgVector, x));
		memset(p + offsetof(LionVecPgVector, x), 0x3c,
			   len - offsetof(LionVecPgVector, x));
	}
	else
		memset(p, 0, len);
	got = lionvec_datum_dim(p, len, half);
	free(p);
	return got;
}

static void
test_datums(void)
{
	bool		ok = true;
	Size		len;

	cur_phase = "datums";
	CHECK(offsetof(LionVecPgVector, x) == 8 && offsetof(LionVecPgHalfVector, x) == 8 &&
		  offsetof(LionVecPgVector, dim) == 4 && offsetof(LionVecPgHalfVector, dim) == 4,
		  "pgvector's header is not as it lays it out");

	/* shorter than the header: never read */
	for (len = 0; len < 8; len++)
		ok &= datum_dim(len, 0, false) == -1 && datum_dim(len, 0, true) == -1;
	CHECK(ok, "a datum shorter than its header is taken for one");

	CHECK(datum_dim(8, 0, false) == 0 && datum_dim(8, 0, true) == 0,
		  "a header alone is not a datum of no dimensions");
	CHECK(datum_dim(8 + 4 * 3, 3, false) == 3 && datum_dim(8 + 4 * 3 - 1, 3, false) == -1 &&
		  datum_dim(8 + 4 * 3 + 4, 3, false) == 3,
		  "a vector's length against its count");
	CHECK(datum_dim(8 + 2 * 3, 3, true) == 3 && datum_dim(8 + 2 * 3 - 1, 3, true) == -1 &&
		  datum_dim(8 + 2 * 3, 3, false) == -1,
		  "a halfvec's length against its count");
	CHECK(datum_dim(8 + 2 * 16000, 16000, true) == 16000 &&
		  datum_dim(8 + 4 * 16000, 16000, false) == 16000 &&
		  datum_dim(8 + 4 * 16000 - 1, 16000, false) == -1,
		  "pgvector's most dimensions");
	CHECK(datum_dim(64, -1, false) == -1 && datum_dim(64, -1, true) == -1 &&
		  datum_dim(64, PG_INT16_MIN, true) == -1,
		  "a negative count is taken");
	CHECK(datum_dim(64, PG_INT16_MAX, false) == -1 && datum_dim(64, PG_INT16_MAX, true) == -1,
		  "a count beyond the bytes is taken");
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

/* lionvec_route()'s scratch, for n regions of k centroids */
static void
route_alloc(LionVecRoute *rt, int n, int k)
{
	rt->key = (double *) xmalloc(sizeof(double) * Max(n, 1));
	rt->order = (int *) xmalloc(sizeof(int) * Max(n, 1));
	rt->pick = (int *) xmalloc(sizeof(int) * Max(n, 1));
	rt->idx = (int *) xmalloc(sizeof(int) * Max(k, 1));
}

static void
route_free(LionVecRoute *rt)
{
	free(rt->key);
	free(rt->order);
	free(rt->pick);
	free(rt->idx);
}

/*
 * k-means as a parallel build runs it (lion_vec.c, DESIGN.md §44.12): its
 * steps, each iteration's rows assigned a chunk of the given size at a time,
 * the chunks taken last first and alternately by two "processes", each with
 * scratch of its own, sharing the centroids' codes - and, given regions r,
 * routed through them, regrouped by the "leader" at the start of each
 * iteration, each process routing in scratch of its own.  Returns the
 * iterations.
 */
static int
kmeans_by_chunks(const Sample *sm, int k, int iters, bool unit, const float *mean,
				 float *cent, LionVecKmeansWork *w, int chunk, LionVecRegions *r,
				 LionVecRegionMaker *mk)
{
	LionVecKmeansWork w2 = *w;
	int			n = sm->s.n;
	int			D = sm->s.dimsp;
	int			nchunks = (n + chunk - 1) / chunk;
	int			made = 0;

	if (r != NULL)
		route_alloc(&w2.route, r->n, k);
	w2.lo = (double *) xmalloc(sizeof(double) * k);
	w2.hi = (double *) xmalloc(sizeof(double) * k);
	w2.cand = (int *) xmalloc(sizeof(int) * k);
	w2.x = falloc(D);
	w2.y = falloc(D);
	w2.codes = (uint8 *) xmalloc(D);
	w2.qc = falloc(D);
	w2.planes = (uint64 *) xmalloc(sizeof(uint64) * (D / 64) * LIONVEC_QBITS);
	lionvec_kmeans_start(&sm->s, k, unit, 42, cent, w->assign);
	while (made < iters)
	{
		int			moved = 0;
		int			c;

		made++;
		lionvec_kmeans_codes(k, D, mean, cent, w);
		if (r != NULL)
			lionvec_regions_make(r, cent, unit, LIONVEC_REGION_SEED(42), mean, mk);
		for (c = nchunks - 1; c >= 0; c--)
			moved += lionvec_kmeans_assign(&sm->s, c * chunk, Min((c + 1) * chunk, n),
										   k, mean, cent, w->assign,
										   c % 2 == 0 ? w : &w2, r);
		if (moved == 0)
			break;
		lionvec_kmeans_update(&sm->s, k, unit, w->assign, cent, w);
	}
	if (r != NULL)
		route_free(&w2.route);
	free(w2.lo);
	free(w2.hi);
	free(w2.cand);
	free(w2.x);
	free(w2.y);
	free(w2.codes);
	free(w2.qc);
	free(w2.planes);
	return made;
}

/* one k-means run, checked for what holds of any: returns the iterations */
static int
kmeans_run(const Sample *sm, int k, int iters, bool unit, const float *mean,
		   float *cent, LionVecKmeansWork *w)
{
	int			D = sm->s.dimsp;
	int			made = lionvec_kmeans(&sm->s, k, iters, unit, 42, mean, cent, w, NULL, NULL);
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

		/*
		 * ... however its rows are shared out: a parallel build's k-means, to
		 * the bit, the assignment too, in chunks of 1, 7 and 32 rows and in
		 * one, with eight centroids a cluster (empty ones split) and with T
		 */
		{
			int		   *assign0 = (int *) xmalloc(sizeof(int) * n);
			int			chunks[4] = {1, 7, 32, n};
			int			kk;

			for (kk = T; kk <= 8 * T; kk += 7 * T)
			{
				int			c;
				int			made0;

				kmeans_work_free(&w);
				kmeans_work(&w, n, kk, D);
				made0 = lionvec_kmeans(&sm.s, kk, 10, unit, 42, mean, cent2, &w, NULL, NULL);
				memcpy(assign0, w.assign, sizeof(int) * n);
				for (c = 0; c < 4; c++)
				{
					int			made1 = kmeans_by_chunks(&sm, kk, 10, unit, mean, cent,
														 &w, chunks[c], NULL, NULL);

					CHECK(made1 == made0 &&
						  memcmp(cent, cent2, sizeof(float) * kk * D) == 0 &&
						  memcmp(w.assign, assign0, sizeof(int) * n) == 0,
						  "k-means by chunks is not k-means");
				}
			}
			free(assign0);
			kmeans_work_free(&w);
			kmeans_work(&w, n, T, D);
			kmeans_run(&sm, T, 50, unit, mean, cent, &w);
		}

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
		lionvec_kmeans(&sm.s, k, 10, true, 7, mean, cent, &w, NULL, NULL);
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
 * The candidates as an insert finds them (lion_vec.c,
 * lionvec_insert_nearest()): a centroid at a time through
 * lionvec_cands_add(), keeping at most maxc - pruning by the threshold
 * so far when full, and growing only when that frees less than half - then
 * those within the final threshold.  Into cand[], in order; returns their
 * number, and *peak the most it kept at once.
 */
static int
stream_candidates(const LionVecCentroids *c, const LionVecQuery *q, int m,
				  int maxc, int *cand, int *peak)
{
	int			nw = c->dimsp / 64;
	int		   *kept = (int *) xmalloc(sizeof(int) * Max(c->k, 1));
	double	   *keptlo = (double *) xmalloc(sizeof(double) * Max(c->k, 1));
	LionVecCandScan cs;
	double		thr;
	int			nc = 0;
	int			n = 0;
	int			i;
	int			j;

	*peak = 0;
	lionvec_cands_begin(&cs, m);
	for (j = 0; j < c->k; j++)
	{
		double		lo;
		double		hi;

		if (!lionvec_cands_add(&cs, q, c->bits + (Size) j * nw, c->popcount[j],
							   c->norm[j], c->alpha1[j], &lo, &hi))
			continue;
		if (nc == maxc)
		{
			int			k2 = 0;

			thr = lionvec_cands_threshold(&cs);
			for (i = 0; i < nc; i++)
				if (keptlo[i] <= thr)
				{
					kept[k2] = kept[i];
					keptlo[k2++] = keptlo[i];
				}
			nc = k2;
			if (nc > maxc / 2)
				maxc *= 2;
		}
		kept[nc] = j;
		keptlo[nc++] = lo;
		*peak = Max(*peak, nc);
	}
	thr = lionvec_cands_threshold(&cs);
	for (i = 0; i < nc; i++)
		if (keptlo[i] <= thr)
			cand[n++] = kept[i];
	free(keptlo);
	free(kept);
	return n;
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
		bool		streamed = true;
		int			maxpeak = 0;
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
			/* an insert's, a centroid at a time: the same, in the same order */
			for (j = 1; j <= m; j += m - 1)
			{
				int			peak;

				nc = lionvec_candidates_n(&cc.c, &q, j, lo, hi, cand);
				nc1 = stream_candidates(&cc.c, &q, j, 4, cand1, &peak);
				streamed &= nc == nc1 && memcmp(cand, cand1, sizeof(int) * nc) == 0;
				maxpeak = Max(maxpeak, peak);
			}
			nc = lionvec_candidates_n(&cc.c, &q, 1, lo, hi, cand);
			nc1 = lionvec_candidates(&cc.c, &q, lo1, hi1, cand1);
			same &= nc == nc1 && memcmp(cand, cand1, sizeof(int) * nc) == 0;
		}
		CHECK(same, "candidates_n of 1 is not candidates");
		CHECK(streamed, "the candidates a centroid at a time are not candidates_n's");
		printf("vec_test: dims %4d: a centroid at a time kept at most %d of %d\n",
			   D, maxpeak, k);
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

/* ---------- the sample's cut, and the centroids' regions ---------- */

/*
 * lionvec_subsample(): exactly m of the n slots, ascending - so distinct,
 * and the rows they name moved down in the reservoir's order - the same from
 * the same seed and another from another, every slot when m is n, and each
 * slot as often as any other.  And lionvec_kmeans_rows(), the m a build
 * cuts to: 128 a partition, never under the floor, never over the rows.
 */
static void
test_subsample(void)
{
	static const int ns[] = {0, 1, 2, 10, 1000, 100003};
	uint32	   *slot = (uint32 *) xmalloc(sizeof(uint32) * 100003);
	uint32	   *slot2 = (uint32 *) xmalloc(sizeof(uint32) * 100003);
	long		count[20];
	bool		ordered = true;
	bool		same = true;
	bool		differ = true;
	bool		all = true;
	bool		even = true;
	int			ni;
	int			s;
	int			i;

	cur_phase = "subsample";
	for (ni = 0; ni < (int) lengthof(ns); ni++)
	{
		int			n = ns[ni];
		int			ms[6];
		int			mi;

		ms[0] = 0;
		ms[1] = Min(1, n);
		ms[2] = n / 3;
		ms[3] = n / 2;
		ms[4] = Max(n - 1, 0);
		ms[5] = n;
		for (mi = 0; mi < 6; mi++)
		{
			int			m = ms[mi];

			lionvec_subsample(n, m, 7, slot);
			for (i = 0; i < m; i++)
				ordered &= slot[i] < (uint32) n && (i == 0 || slot[i - 1] < slot[i]);
			if (m == n)
				for (i = 0; i < m; i++)
					all &= slot[i] == (uint32) i;
			lionvec_subsample(n, m, 7, slot2);
			same &= memcmp(slot, slot2, sizeof(uint32) * m) == 0;
			if (m >= 10 && n - m >= 10)
			{
				lionvec_subsample(n, m, 8, slot2);
				differ &= memcmp(slot, slot2, sizeof(uint32) * m) != 0;
			}
		}
	}
	CHECK(ordered, "the cut is not m distinct slots in ascending order");
	CHECK(all, "a cut of every slot is not every slot");
	CHECK(same, "the cut is not repeatable");
	CHECK(differ, "another seed made the same cut");

	/* 5 of 20 from 8,000 seeds: 2,000 draws a slot, give or take 39 */
	memset(count, 0, sizeof(count));
	for (s = 0; s < 8000; s++)
	{
		lionvec_subsample(20, 5, (uint64) s * 0x9E3779B97F4A7C15ULL, slot);
		for (i = 0; i < 5; i++)
			count[slot[i]]++;
	}
	for (i = 0; i < 20; i++)
		even &= labs(count[i] - 2000) < 200;
	CHECK(even, "a slot is drawn more often than another");

	CHECK(lionvec_kmeans_rows(0, 0) == 0 && lionvec_kmeans_rows(10, 0) == 10 &&
		  lionvec_kmeans_rows(10, 10) == 10 &&
		  lionvec_kmeans_rows(LIONVEC_SAMPLE_MIN, 1) == LIONVEC_SAMPLE_MIN &&
		  lionvec_kmeans_rows(LIONVEC_SAMPLE_MIN + 1, 1) == LIONVEC_SAMPLE_MIN &&
		  lionvec_kmeans_rows(1000000, 100) == LIONVEC_SAMPLE_MIN &&
		  lionvec_kmeans_rows(1000000, 512) == LIONVEC_SAMPLE_MIN &&
		  lionvec_kmeans_rows(1000000, 513) == 513 * LIONVEC_SAMPLE_PER_PART_MAX &&
		  lionvec_kmeans_rows(1000000, 1000) == 128000 &&
		  lionvec_kmeans_rows(100000, 1000) == 100000 &&
		  lionvec_kmeans_rows(1 << 24, 262144) == 1 << 24 &&
		  lionvec_kmeans_rows(1 << 24, 100000) == 12800000,
		  "the rows k-means is given are not 128 a partition, the floor at least, the rows at most");
	free(slot2);
	free(slot);
}

/* ---------- the partitions a build makes, and its sample ---------- */

/*
 * lionvec_parts() and what the build rests on (DESIGN.md §44.3, §44.8): the
 * rule's figures - one partition below 10,000 rows, 8 sqrt(n) above, at most
 * a sixteenth of the sample, the option's count at most the sample's rows,
 * LIONVEC_MAX_PARTS past 1.07 billion rows - and the reservoir's at the
 * figures DESIGN.md quotes.  A reservoir of every row is never short, and
 * the cap of 2^24 rows takes no partition away.  With smax fixed, short
 * stays short as the rows grow, for every count of rows to 200,000 and
 * then every 0.05% to 4 billion: the first pass fails at the first row that
 * makes it so, sure that the table's rows would too.  And the
 * maintenance_work_mem the error names (lionvec_parts_sample(),
 * lionvec_sample_mem()) makes the partitions wanted, and a kilobyte less
 * does not, but where any sample makes them: the one partition of a table
 * under 10,000 rows, or of an option of 1.
 */
static void
test_partitions(void)
{
	static const int asks[] = {0, 1, 2, 20, 600, 5000, 30000, LIONVEC_MAX_PARTS};
	static const int64 smaxes[] = {1, 16, 340, 481, 3971, 7710, 21788, 348617,
	1 << 22, LIONVEC_SAMPLE_MAX};
	static const int dimsps[] = {64, 128, 576, 1536, 8128};
	static const double rows[] = {1, 15, 16, 17, 9999, 10000, 10001, 16383,
		16384, 28941, 28943, 40000, 1e6, 7.4e6, 1e8, 1e9, 1.08e9, 4e9};
	bool		every = true;
	bool		capfree = true;
	bool		stays = true;
	bool		enough = true;
	bool		less = true;
	int			ai;
	int			si;
	int			di;
	int			ri;

	cur_phase = "partitions";
	CHECK(lionvec_parts(0, 0.0, 0) == 0 && lionvec_parts(0, 5.0, 0) == 0 &&
		  lionvec_parts(20, 0.0, 0) == 0 && lionvec_parts(0, 1.0, 1) == 1 &&
		  lionvec_parts(0, 9999.0, 9999) == 1 && lionvec_parts(0, 9999.0, 1) == 1 &&
		  lionvec_parts(0, 10000.0, 10000) == 625 &&
		  lionvec_parts(0, 16384.0, 16384) == 1024 &&
		  lionvec_parts(0, 1e6, 1000000) == 8000 &&
		  lionvec_parts(0, 1e6, 21788) == 1361 &&
		  lionvec_parts(0, 1e9, LIONVEC_SAMPLE_MAX) == 252982 &&
		  lionvec_parts(0, 4e9, LIONVEC_SAMPLE_MAX) == LIONVEC_MAX_PARTS &&
		  lionvec_parts(500, 300.0, 300) == 300 &&
		  lionvec_parts(30000, 40000.0, 21788) == 21788 &&
		  lionvec_parts(30000, 40000.0, 40000) == 30000,
		  "the partitions are not the rule's");
	CHECK(lionvec_sample_max(65536, 1536) == 21788 &&
		  lionvec_sample_max(1048576, 1536) == 348617 &&
		  lionvec_sample_max(64, 64) == 481 &&
		  lionvec_sample_max(0, 1536) == 1 &&
		  lionvec_sample_max(INT64CONST(54525952), 1536) == LIONVEC_SAMPLE_MAX &&
		  lionvec_sample_max(PG_INT32_MAX, 64) == LIONVEC_SAMPLE_MAX,
		  "the reservoir is not what half of maintenance_work_mem holds, to its cap");
	/* a billion rows: 252,982 partitions, 4,047,712 sample rows, 11,890 MB */
	CHECK(lionvec_parts_wanted(0, 1e9) == 252982 &&
		  lionvec_parts_sample(0, 252982) == 4047712 &&
		  lionvec_parts_sample(30000, 30000) == 30000 &&
		  lionvec_sample_mem(4047712, 1536) == 12174759 &&
		  lionvec_parts_short(0, 1e9, 21788) &&
		  lionvec_parts_short(0, 40000.0, 21788) &&
		  !lionvec_parts_short(0, 28000.0, 21788) &&
		  !lionvec_parts_short(0, 1e6, 348617) &&
		  lionvec_parts_short(30000, 40000.0, 21788) &&
		  !lionvec_parts_short(30000, 21788.0, 21788),
		  "a billion rows' or the reproduction's figures are not DESIGN.md's");

	for (ai = 0; ai < (int) lengthof(asks); ai++)
	{
		int			ask = asks[ai];

		for (ri = 0; ri < (int) lengthof(rows); ri++)
		{
			double		n = rows[ri];
			int			w = lionvec_parts_wanted(ask, n);

			every &= w == lionvec_parts(ask, n, (int64) n) &&
				!lionvec_parts_short(ask, n, (int64) n);
			capfree &= n < LIONVEC_SAMPLE_MAX ||
				w == lionvec_parts(ask, n, (int64) LIONVEC_SAMPLE_MAX);
			for (di = 0; di < (int) lengthof(dimsps); di++)
			{
				int64		need = lionvec_parts_sample(ask, w);
				int64		mem = lionvec_sample_mem(need, dimsps[di]);

				enough &= !lionvec_parts_short(ask, n,
											   lionvec_sample_max(mem, dimsps[di]));
				/* a reservoir is a row at least: one row's is any */
				if (need > 1 && (ask > 0 || n >= LIONVEC_MIN_AUTO_ROWS))
					less &= lionvec_parts_short(ask, n,
												lionvec_sample_max(mem - 1, dimsps[di]));
			}
		}
		for (si = 0; si < (int) lengthof(smaxes); si++)
		{
			int64		smax = smaxes[si];
			bool		was = false;
			double		n;

			for (n = 1.0; n <= 4e9; n = n < 200000.0 ? n + 1.0 : floor(n * 1.0005))
			{
				bool		now = lionvec_parts_short(ask, n, smax);

				stays &= now || !was;
				every &= !now || n > (double) smax;
				was = now;
			}
		}
	}
	CHECK(every, "a sample of every row makes fewer partitions than the rows want");
	CHECK(capfree, "the reservoir's cap takes a partition away");
	CHECK(stays, "more rows than a short sample's are not short too");
	CHECK(enough, "the maintenance_work_mem named does not make the partitions wanted");
	CHECK(less, "a kilobyte less than the maintenance_work_mem named makes them too");
}

/* k centroids' codes, of which sub holds those idx[0 .. n) names, in that order */
static void
cent_codes_sub(CentCodes *sub, const CentCodes *cc, const int *idx, int n)
{
	int			nw = cc->c.dimsp / 64;
	int			i;

	for (i = 0; i < n; i++)
	{
		int			j = idx[i];

		memcpy(sub->bits + (Size) i * nw, cc->bits + (Size) j * nw,
			   sizeof(uint64) * nw);
		sub->pop[i] = cc->pop[j];
		sub->norm[i] = cc->norm[j];
		sub->alpha1[i] = cc->alpha1[j];
	}
	sub->c.k = n;
	sub->c.dimsp = cc->c.dimsp;
	sub->c.bits = sub->bits;
	sub->c.popcount = sub->pop;
	sub->c.norm = sub->norm;
	sub->c.alpha1 = sub->alpha1;
}

/*
 * lionvec_candidates_among(): over the centroids idx names, in any order,
 * the candidates and bounds lionvec_candidates_n() finds among those
 * centroids alone - the bounds by their places in idx, the candidates by
 * their numbers - and with no idx, candidates_n's own.
 */
static void
test_candidates_among(void)
{
	static const int dims[] = {64, 320};
	int			di;

	cur_phase = "candidates_among";
	for (di = 0; di < (int) lengthof(dims); di++)
	{
		int			D = dims[di];
		int			k = 300;
		int			T = 20;
		float	   *centers = falloc(T * D);
		float	   *cent = falloc(k * D);
		float	   *x = falloc(D);
		double	   *lo = (double *) xmalloc(sizeof(double) * k);
		double	   *hi = (double *) xmalloc(sizeof(double) * k);
		double	   *lo1 = (double *) xmalloc(sizeof(double) * k);
		double	   *hi1 = (double *) xmalloc(sizeof(double) * k);
		int		   *cand = (int *) xmalloc(sizeof(int) * k);
		int		   *cand1 = (int *) xmalloc(sizeof(int) * k);
		int		   *idx = (int *) xmalloc(sizeof(int) * k);
		uint32	   *slot = (uint32 *) xmalloc(sizeof(uint32) * k);
		LionVecQuery q;
		CentCodes	cc;
		CentCodes	sub;
		bool		same = true;
		bool		whole = true;
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
		cent_codes(&sub, cent, k, D, NULL);

		for (trial = 0; trial < 200; trial++)
		{
			int			c = (int) (rnd64() % k);
			int			n = 1 + (int) (rnd64() % k);
			int			m = trial % 2 == 0 ? 1 : LIONVEC_SOAR_CANDS + 1;
			int			nc;
			int			nc1;

			for (i = 0; i < D; i++)
				x[i] = cent[(Size) c * D + i] + (float) (rnd_gauss() * 0.4);
			lionvec_query_init(&q, LIONVEC_L2, NULL, x, LIONVEC_ASSIGN_EPS);

			/* n of them, ascending as a route's are, or shuffled */
			lionvec_subsample(k, n, (uint64) trial, slot);
			for (i = 0; i < n; i++)
				idx[i] = (int) slot[i];
			if (trial % 4 >= 2)
				for (i = n - 1; i > 0; i--)
				{
					int			r = (int) (rnd64() % (i + 1));
					int			t = idx[i];

					idx[i] = idx[r];
					idx[r] = t;
				}
			cent_codes_sub(&sub, &cc, idx, n);
			nc = lionvec_candidates_among(&cc.c, idx, n, &q, m, lo, hi, cand);
			nc1 = lionvec_candidates_n(&sub.c, &q, m, lo1, hi1, cand1);
			same &= nc == nc1 &&
				memcmp(lo, lo1, sizeof(double) * n) == 0 &&
				memcmp(hi, hi1, sizeof(double) * n) == 0;
			for (i = 0; i < Min(nc, nc1); i++)
				same &= cand[i] == idx[cand1[i]];

			/* no idx: every centroid, as candidates_n */
			nc = lionvec_candidates_among(&cc.c, NULL, k, &q, m, lo, hi, cand);
			nc1 = lionvec_candidates_n(&cc.c, &q, m, lo1, hi1, cand1);
			whole &= nc == nc1 && memcmp(cand, cand1, sizeof(int) * nc) == 0 &&
				memcmp(lo, lo1, sizeof(double) * k) == 0;
		}
		CHECK(same, "candidates among some centroids are not candidates_n of those alone");
		CHECK(whole, "candidates among every centroid are not candidates_n's");

		cent_codes_free(&sub);
		cent_codes_free(&cc);
		query_free(&q);
		free(slot);
		free(idx);
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

/* regions of k centroids, and what lionvec_regions_make() works in */
typedef struct RegionBufs
{
	LionVecRegions r;
	LionVecRegionMaker mk;
} RegionBufs;

static void
region_bufs(RegionBufs *b, int k, int D)
{
	int			n = lionvec_region_count(k);

	memset(b, 0, sizeof(*b));
	b->r.n = n;
	b->r.k = k;
	b->r.dimsp = D;
	b->r.bits = (uint64 *) xmalloc(sizeof(uint64) * (Size) n * (D / 64));
	b->r.popcount = (uint16 *) xmalloc(sizeof(uint16) * n);
	b->r.norm = falloc(n);
	b->r.alpha1 = falloc(n);
	b->r.first = (int *) xmalloc(sizeof(int) * (n + 1));
	b->r.member = (int *) xmalloc(sizeof(int) * k);
	/* k-means of k rows, the centroids, into n */
	kmeans_work(&b->mk.w, k, n, D);
	b->mk.codes = (uint8 *) xmalloc((Size) k * D);
	b->mk.scale = falloc(k);
	b->mk.cent = falloc(n * D);
}

static void
region_bufs_free(RegionBufs *b)
{
	free(b->r.bits);
	free(b->r.popcount);
	free(b->r.norm);
	free(b->r.alpha1);
	free(b->r.first);
	free(b->r.member);
	kmeans_work_free(&b->mk.w);
	free(b->mk.codes);
	free(b->mk.scale);
	free(b->mk.cent);
}

/* the regions are the same, to the bit */
static bool
regions_same(const LionVecRegions *a, const LionVecRegions *b)
{
	return a->n == b->n && a->k == b->k &&
		memcmp(a->first, b->first, sizeof(int) * (a->n + 1)) == 0 &&
		memcmp(a->member, b->member, sizeof(int) * a->k) == 0 &&
		memcmp(a->bits, b->bits, sizeof(uint64) * (Size) a->n * (a->dimsp / 64)) == 0 &&
		memcmp(a->popcount, b->popcount, sizeof(uint16) * a->n) == 0 &&
		memcmp(a->norm, b->norm, sizeof(float) * a->n) == 0 &&
		memcmp(a->alpha1, b->alpha1, sizeof(float) * a->n) == 0;
}

/*
 * lionvec_regions_pick()'s answer the obvious way: the regions with members
 * by key, ties to the lower, NaN as infinite; the first beam, then more
 * while they hold fewer than need; ascending.
 */
static int
pick_reference(const double *key, const int *first, int n, int beam, int need,
			   int *pick)
{
	int		   *by = (int *) xmalloc(sizeof(int) * n);
	int			nb = 0;
	int			got = 0;
	int			members = 0;
	int			g;
	int			i;

	for (g = 0; g < n; g++)
	{
		double		kg = isnan(key[g]) ? INFINITY : key[g];

		if (first[g + 1] == first[g])
			continue;
		for (i = nb; i > 0; i--)
		{
			int			h = by[i - 1];
			double		kh = isnan(key[h]) ? INFINITY : key[h];

			if (!(kh > kg))
				break;
			by[i] = by[i - 1];
		}
		by[i] = g;
		nb++;
	}
	for (i = 0; i < nb && (got < beam || members < need); i++)
	{
		pick[got++] = by[i];
		members += first[by[i] + 1] - first[by[i]];
	}
	free(by);
	/* ascending */
	for (i = 1; i < got; i++)
	{
		int			v = pick[i];
		int			j = i;

		for (; j > 0 && pick[j - 1] > v; j--)
			pick[j] = pick[j - 1];
		pick[j] = v;
	}
	return got;
}

/*
 * The regions' counts; lionvec_regions_pick() against the obvious way, with
 * empty regions, ties and keys that are not numbers; then, over centroids
 * k-means made of clustered rows: lionvec_regions_make()'s members
 * partition the centroids, each region's ascending, each centroid in the
 * region nearest it (as the build routes), the same regions every time; the
 * partitions numbered region by region (lion_vec.c) are each region's run
 * and route every row as before, so that an insert reading the runs finds
 * what the build did; a route's candidates are among its regions' members
 * and hold every exact candidate there is among them; and the nearest
 * centroid a route finds is the nearest of all for 97% of rows or more.
 */
static void
test_regions(void)
{
	int			D = 128;
	int			T = 40;
	int			nrows = 12000;
	int			nq = 3000;
	int			k = 600;
	float	   *rows = falloc(nrows * D);
	float	   *queries = falloc(nq * D);
	float	   *dec = falloc(nrows * D);
	float	   *mean = falloc(D);
	float	   *cent = falloc(k * D);
	float	   *cent2 = falloc(k * D);
	float	   *y = falloc(D);
	double	   *lo = (double *) xmalloc(sizeof(double) * k);
	double	   *hi = (double *) xmalloc(sizeof(double) * k);
	int		   *cand = (int *) xmalloc(sizeof(int) * k);
	int		   *cand2 = (int *) xmalloc(sizeof(int) * k);
	int		   *ecand = (int *) xmalloc(sizeof(int) * k);
	bool	   *inpick = (bool *) xmalloc(sizeof(bool) * k);
	bool	   *seen = (bool *) xmalloc(sizeof(bool) * k);
	int		   *ident = (int *) xmalloc(sizeof(int) * k);
	LionVecKmeansWork w;
	LionVecRoute rt;
	LionVecQuery q;
	CentCodes	cc;
	CentCodes	cc2;
	RegionBufs	rb;
	RegionBufs	rb2;
	LionVecRegions rn;
	Sample		sm;
	bool		ok;
	int			n;
	int			g;
	int			i;
	int			j;

	cur_phase = "regions";

	/* the counts: none below LIONVEC_REGION_MIN, then ceil(sqrt(k)) */
	CHECK(lionvec_region_count(0) == 0 && lionvec_region_count(1) == 0 &&
		  lionvec_region_count(LIONVEC_REGION_MIN - 1) == 0,
		  "regions of fewer than LIONVEC_REGION_MIN centroids");
	CHECK(lionvec_region_count(64) == 8 && lionvec_region_count(65) == 9 &&
		  lionvec_region_count(2717) == 53 && lionvec_region_count(262144) == 512,
		  "the regions of k centroids are not ceil(sqrt(k))");
	ok = true;
	for (n = 1; n <= 1000; n++)
	{
		int			m;

		for (m = 1; m <= LIONVEC_SOAR_CANDS + 1; m += LIONVEC_SOAR_CANDS)
		{
			int			b = lionvec_region_beam(n, m);

			ok &= b >= 1 && b <= n &&
				(n == 1 || b >= lionvec_region_beam(n - 1, m)) &&
				b >= lionvec_region_beam(n, 1);
		}
	}
	CHECK(ok, "a beam of none, of more regions than there are, shrinking, or narrower for more nearest");

	/* the pick, against the obvious way */
	{
		int			nr = 30;
		double	   *key = (double *) xmalloc(sizeof(double) * nr);
		int		   *first = (int *) xmalloc(sizeof(int) * (nr + 1));
		int		   *order = (int *) xmalloc(sizeof(int) * nr);
		int		   *pick = (int *) xmalloc(sizeof(int) * nr);
		int		   *pick2 = (int *) xmalloc(sizeof(int) * nr);
		bool		same = true;
		int			trial;

		for (trial = 0; trial < 2000; trial++)
		{
			int			beam = 1 + (int) (rnd64() % 8);
			int			need = (int) (rnd64() % 40);
			int			total;
			int			np;
			int			np2;

			first[0] = 0;
			for (g = 0; g < nr; g++)
			{
				/* a third empty; keys from few values, so that they tie */
				int			c = rnd64() % 3 == 0 ? 0 : 1 + (int) (rnd64() % 4);
				int			kind = (int) (rnd64() % 10);

				first[g + 1] = first[g] + c;
				key[g] = kind == 0 ? NAN : kind == 1 ? INFINITY :
					(double) (rnd64() % 6);
			}
			total = first[nr];
			np = lionvec_regions_pick(key, first, nr, beam, need, order, pick);
			np2 = pick_reference(key, first, nr, beam, need, pick2);
			same &= np == np2 && memcmp(pick, pick2, sizeof(int) * np) == 0;
			/* need more than they all have: all of them */
			if (trial % 10 == 0)
			{
				int			nonempty = 0;

				for (g = 0; g < nr; g++)
					nonempty += first[g + 1] > first[g];
				np = lionvec_regions_pick(key, first, nr, beam, total + 1, order, pick);
				same &= np == nonempty;
			}
		}
		CHECK(same, "the regions picked are not the nearest with members");
		free(pick2);
		free(pick);
		free(order);
		free(first);
		free(key);
	}

	/* centroids as a build makes them: exact k-means of clustered rows */
	make_clusters(rows, nrows, queries, nq, D, T);
	sample_make(&sm, rows, nrows, D);
	for (i = 0; i < nrows; i++)
		lionvec_sample_row(&sm.s, i, dec + (Size) i * D);
	for (j = 0; j < D; j++)
	{
		double		s = 0.0;

		for (i = 0; i < nrows; i++)
			s += dec[(Size) i * D + j];
		mean[j] = (float) (s / nrows);
	}
	kmeans_work(&w, nrows, k, D);
	(void) lionvec_kmeans(&sm.s, k, 5, false, 42, mean, cent, &w, NULL, NULL);
	kmeans_work_free(&w);

	/* the regions: a partition of the centroids, each in its nearest */
	region_bufs(&rb, k, D);
	region_bufs(&rb2, k, D);
	n = rb.r.n;
	CHECK(n == 25, "600 centroids are not in 25 regions");
	lionvec_regions_make(&rb.r, cent, false, 43, mean, &rb.mk);
	ok = rb.r.first[0] == 0 && rb.r.first[n] == k;
	memset(seen, 0, sizeof(bool) * k);
	for (g = 0; g < n; g++)
	{
		ok &= rb.r.first[g + 1] >= rb.r.first[g];
		for (i = rb.r.first[g]; i < rb.r.first[g + 1]; i++)
		{
			int			c = rb.r.member[i];

			ok &= c >= 0 && c < k && !seen[c];
			if (c >= 0 && c < k)
				seen[c] = true;
			ok &= i == rb.r.first[g] || rb.r.member[i - 1] < c;
		}
	}
	CHECK(ok, "the regions' members are not the centroids, each once, ascending");
	{
		int			near = 0;

		for (g = 0; g < n; g++)
			for (i = rb.r.first[g]; i < rb.r.first[g + 1]; i++)
			{
				int			c = rb.r.member[i];

				/* the centroid as k-means of the centroids has it: its codes */
				lionvec_sample_row(&(LionVecSample)
								   {k, D, rb.mk.codes, rb.mk.scale}, c, y);
				near += nearest_exact(y, rb.mk.cent, n, D, NULL) == g;
			}
		CHECK(near >= k * 99 / 100, "a centroid is not in the region nearest it");
	}
	lionvec_regions_make(&rb2.r, cent, false, 43, mean, &rb2.mk);
	CHECK(regions_same(&rb.r, &rb2.r), "the same centroids made other regions");

	/*
	 * Numbered region by region: partition j is centroid member[j], so
	 * region g's are the run first[g] .. first[g + 1] - 1, and its members
	 * the identity; routing through them finds the same centroids, in the
	 * same order, under their new numbers.
	 */
	for (j = 0; j < k; j++)
	{
		memcpy(cent2 + (Size) j * D, cent + (Size) rb.r.member[j] * D,
			   sizeof(float) * D);
		ident[j] = j;
	}
	rn = rb.r;
	rn.member = ident;
	cent_codes(&cc, cent, k, D, mean);
	cent_codes(&cc2, cent2, k, D, mean);
	route_alloc(&rt, n, k);
	query_alloc(&q, D);
	{
		int			m;
		bool		renumbered = true;
		bool		among = true;
		long		subset = 0;
		long		match = 0;
		long		nc_route = 0;
		long		nc_exact = 0;

		for (m = 1; m <= LIONVEC_SOAR_CANDS + 1; m += LIONVEC_SOAR_CANDS)
		{
			for (i = 0; i < nq; i++)
			{
				const float *x = queries + (Size) i * D;
				int			nc;
				int			nc2;
				int			ne;
				int			np;
				int			best = -1;
				double		bestd = INFINITY;
				bool		sub = true;
				int			c;

				lionvec_query_init(&q, LIONVEC_L2, mean, x, LIONVEC_ASSIGN_EPS);
				nc = lionvec_route(&rb.r, &cc.c, &q, m, &rt, lo, hi, cand);
				np = lionvec_regions_pick(rt.key, rb.r.first, n, lionvec_region_beam(n, m), m,
										  rt.order, rt.pick);
				memset(inpick, 0, sizeof(bool) * k);
				for (g = 0; g < np; g++)
					for (j = rb.r.first[rt.pick[g]]; j < rb.r.first[rt.pick[g] + 1]; j++)
						inpick[rb.r.member[j]] = true;
				among &= nc >= Min(m, k);

				/* renumbered: the same centroids, the same order */
				nc2 = lionvec_route(&rn, &cc2.c, &q, m, &rt, lo, hi, cand2);
				renumbered &= nc2 == nc;
				for (c = 0; c < Min(nc, nc2); c++)
					renumbered &= rb.r.member[cand2[c]] == cand[c] &&
						(c == 0 || cand2[c - 1] < cand2[c]);

				/*
				 * the exact candidates among the regions' members are routed
				 * ones, and the routed ones members; all of them exact ones
				 * unless the beam missed one of the m nearest
				 */
				ne = lionvec_candidates_n(&cc.c, &q, m, lo, hi, ecand);
				memset(seen, 0, sizeof(bool) * k);
				for (c = 0; c < nc; c++)
				{
					among &= inpick[cand[c]];
					seen[cand[c]] = true;
				}
				for (c = 0; c < ne; c++)
					among &= !inpick[ecand[c]] || seen[ecand[c]];
				memset(seen, 0, sizeof(bool) * k);
				for (c = 0; c < ne; c++)
					seen[ecand[c]] = true;
				for (c = 0; c < nc; c++)
					sub &= seen[cand[c]];
				subset += sub;
				nc_route += nc;
				nc_exact += ne;

				/* the nearest, by the exact distance, as k-means assigns */
				for (c = 0; c < nc; c++)
				{
					double		d = 0.0;

					for (j = 0; j < D; j++)
					{
						double		t = (double) x[j] - cent[(Size) cand[c] * D + j];

						d += t * t;
					}
					if (best < 0 || d < bestd)
					{
						best = cand[c];
						bestd = d;
					}
				}
				if (m == 1)
					match += best == nearest_exact(x, cent, k, D, NULL);
			}
		}
		CHECK(renumbered, "numbered region by region, a row is routed elsewhere");
		CHECK(among, "a route's candidates are not its regions' exact ones");
		CHECK(subset >= 2L * nq * 97 / 100,
			  "a route's candidates are not the exact candidates for 97% of rows");
		CHECK(match >= (long) nq * 97 / 100,
			  "a route's nearest is not the nearest for 97% of rows");
		printf("vec_test: regions: %d of %d centroids, beam %d: the nearest for %.2f%% of rows, "
			   "candidates exact ones for %.2f%%, %.1f routed and %.1f exact candidates a row\n",
			   n, k, lionvec_region_beam(n, 1), 100.0 * match / nq, 100.0 * subset / (2 * nq),
			   (double) nc_route / (2 * nq), (double) nc_exact / (2 * nq));
	}

	/* an empty region: never picked, and the route still finds m */
	{
		LionVecRegions re = rb.r;
		int		   *first = (int *) xmalloc(sizeof(int) * (n + 1));
		bool		fine = true;

		/* region 0's members given to region 1 */
		memcpy(first, rb.r.first, sizeof(int) * (n + 1));
		first[1] = 0;
		re.first = first;
		for (i = 0; i < 200; i++)
		{
			int			nc;
			int			np;
			int			c;

			lionvec_query_init(&q, LIONVEC_L2, mean, queries + (Size) i * D,
							   LIONVEC_ASSIGN_EPS);
			nc = lionvec_route(&re, &cc.c, &q, LIONVEC_SOAR_CANDS + 1, &rt, lo, hi,
							   cand);
			np = lionvec_regions_pick(rt.key, first, n, lionvec_region_beam(n, LIONVEC_SOAR_CANDS + 1),
									  LIONVEC_SOAR_CANDS + 1, rt.order, rt.pick);
			fine &= nc >= LIONVEC_SOAR_CANDS + 1;
			for (c = 0; c < np; c++)
				fine &= rt.pick[c] != 0;
		}
		CHECK(fine, "an empty region picked, or too few candidates");
		free(first);
	}

	/*
	 * k-means routed through regions: by chunks, in two "processes", the
	 * same to the bit, the regions too; repeatable; and as good as exact
	 * k-means, by the rows' squared distances from their centroids
	 */
	{
		LionVecKmeansWork w2;
		int		   *assign0 = (int *) xmalloc(sizeof(int) * nrows);
		int			chunks[3] = {1, 32, nrows};
		int			unit;

		for (unit = 0; unit <= 1; unit++)
		{
			int			made0;
			int			c;
			double		cost[2] = {0.0, 0.0};
			int			pass;

			kmeans_work(&w, nrows, k, D);
			route_alloc(&w.route, n, k);
			made0 = lionvec_kmeans(&sm.s, k, 6, unit, 42, mean, cent2, &w, &rb.r,
								   &rb.mk);
			memcpy(assign0, w.assign, sizeof(int) * nrows);
			for (c = 0; c < 3; c++)
			{
				int			made1 = kmeans_by_chunks(&sm, k, 6, unit, mean, cent, &w,
													 chunks[c], &rb2.r, &rb2.mk);

				CHECK(made1 == made0 &&
					  memcmp(cent, cent2, sizeof(float) * k * D) == 0 &&
					  memcmp(w.assign, assign0, sizeof(int) * nrows) == 0 &&
					  regions_same(&rb.r, &rb2.r),
					  "routed k-means by chunks is not routed k-means");
			}
			(void) lionvec_kmeans(&sm.s, k, 6, unit, 42, mean, cent, &w, &rb.r,
								  &rb.mk);
			CHECK(memcmp(cent, cent2, sizeof(float) * k * D) == 0,
				  "routed k-means is not repeatable");

			/* no regions (fewer than LIONVEC_REGION_MIN): exact k-means */
			{
				LionVecRegions none = rb.r;

				none.n = 0;
				(void) lionvec_kmeans(&sm.s, k, 6, unit, 42, mean, cent, &w, &none,
									  &rb.mk);
				memcpy(cent2, cent, sizeof(float) * k * D);
				(void) lionvec_kmeans(&sm.s, k, 6, unit, 42, mean, cent, &w, NULL,
									  NULL);
				CHECK(memcmp(cent, cent2, sizeof(float) * k * D) == 0,
					  "k-means with no regions is not exact k-means");
			}

			/* the cost of each: exact, then routed */
			for (pass = 0; pass < 2; pass++)
			{
				(void) lionvec_kmeans(&sm.s, k, 6, unit, 42, mean, cent, &w,
									  pass == 0 ? NULL : &rb.r,
									  pass == 0 ? NULL : &rb.mk);
				for (i = 0; i < nrows; i++)
				{
					double		d;

					(void) nearest_exact(dec + (Size) i * D, cent, k, D, &d);
					cost[pass] += d;
				}
			}
			CHECK(cost[1] <= cost[0] * 1.02, "routed k-means is worse than exact k-means");
			printf("vec_test: regions: k-means%s of %d rows into %d: routed %.4f of exact's cost\n",
				   unit ? " (spherical)" : "", nrows, k, cost[1] / cost[0]);
			route_free(&w.route);
			kmeans_work_free(&w);
		}
		(void) w2;
		free(assign0);
	}

	query_free(&q);
	route_free(&rt);
	cent_codes_free(&cc2);
	cent_codes_free(&cc);
	region_bufs_free(&rb2);
	region_bufs_free(&rb);
	sample_free(&sm);
	free(ident);
	free(seen);
	free(inpick);
	free(ecand);
	free(cand2);
	free(cand);
	free(hi);
	free(lo);
	free(y);
	free(cent2);
	free(cent);
	free(mean);
	free(dec);
	free(queries);
	free(rows);
}

/* for qsort(): ints ascending */
static int
int_cmp(const void *a, const void *b)
{
	int			x = *(const int *) a;
	int			y = *(const int *) b;

	return x < y ? -1 : x > y;
}

/*
 * Prefetch's arithmetic (DESIGN.md §44.4, "Prefetch"): the items a CODE
 * page holds - 35 of 1536 dimensions, the page's 8,152 bytes of room over
 * 224-byte items and their line pointers - and the pages a chain of so
 * many items takes, one for none and never wrapping; the window a reader
 * asks for, bounded by its distance, its run's end and the relation, and
 * empty past it, at block numbers next to 2^32; and lionvec_least() against
 * lionvec_rank(), the same set of places for keys with ties, NaNs and
 * infinities, of lengths up to 300 and every m up to past them, each kept
 * key no later than every key left out, and put in order by
 * lionvec_least_order(), lionvec_rank()'s order.
 */
static void
test_prefetch(void)
{
	bool		ok = true;
	bool		okord = true;
	int			trial;

	cur_phase = "prefetch";
	CHECK(lionvec_items_per_page(8152, 228) == 35 &&
		  lionvec_items_per_page(8152, 8152) == 1 &&
		  lionvec_items_per_page(100, 228) == 1 &&
		  lionvec_items_per_page(100, 0) == 1 &&
		  lionvec_items_per_page(8152, 4 + 8) == 679,
		  "items a page: the room over an item, at least one");
	CHECK(lionvec_chain_pages(0, 35) == 1 && lionvec_chain_pages(1, 35) == 1 &&
		  lionvec_chain_pages(35, 35) == 1 && lionvec_chain_pages(36, 35) == 2 &&
		  lionvec_chain_pages(84, 35) == 3 && lionvec_chain_pages(70, 35) == 2 &&
		  lionvec_chain_pages(5, 0) == 5 && lionvec_chain_pages(5, -3) == 5 &&
		  lionvec_chain_pages(PG_UINT32_MAX, 1) == PG_UINT32_MAX &&
		  lionvec_chain_pages(PG_UINT32_MAX, 35) == 122713352,
		  "chain pages: the items over a page's, rounded up, at least one");
	CHECK(lionvec_prefetch_end(10, 4, 100) == 15 &&
		  lionvec_prefetch_end(10, 4, 13) == 13 &&
		  lionvec_prefetch_end(10, 4, 11) == 11 &&
		  lionvec_prefetch_end(10, 4, 5) == 11 &&
		  lionvec_prefetch_end(10, 0, 100) == 11 &&
		  lionvec_prefetch_end(10, -7, 100) == 11 &&
		  lionvec_prefetch_end(0, 64, 1) == 1,
		  "prefetch window: dist pages after blk, short of end, never before blk + 1");
	CHECK(lionvec_prefetch_end(0xFFFFFFFEu, 64, 0xFFFFFFFFu) == 0xFFFFFFFFu &&
		  lionvec_prefetch_end(0xFFFFFFFDu, 1, (uint64) PG_UINT32_MAX + 1) == 0xFFFFFFFFu &&
		  lionvec_prefetch_end(0xFFFFFFFFu, 64, 0xFFFFFFFFu) == (uint64) PG_UINT32_MAX + 1,
		  "prefetch window: no block number wraps");

	for (trial = 0; trial < 2000; trial++)
	{
		int			n = (int) (rnd64() % 301);
		int			m = (int) (rnd64() % 80);
		int			want = Min(n, m);
		double	   *key = (double *) xmalloc(sizeof(double) * Max(n, 1));
		int		   *idx = (int *) xmalloc(sizeof(int) * Max(want, 1));
		int		   *ref = (int *) xmalloc(sizeof(int) * Max(want, 1));
		bool	   *in = (bool *) xmalloc(sizeof(bool) * Max(n, 1));
		int			r;
		int			i;
		int			j;

		for (i = 0; i < n; i++)
		{
			uint64		u = rnd64() % 20;

			key[i] = u == 0 ? NAN : u == 1 ? INFINITY : u == 2 ? -INFINITY :
				(double) (rnd64() % 9);
		}
		r = lionvec_least(key, n, m, idx);
		ok &= r == want && lionvec_rank(key, n, m, ref) == want;
		if (r == want)
		{
			int		   *ord = (int *) xmalloc(sizeof(int) * Max(r, 1));

			/* ordered, lionvec_rank()'s order to the place */
			memcpy(ord, idx, sizeof(int) * r);
			lionvec_least_order(key, ord, r);
			for (j = 0; j < r; j++)
				okord &= ord[j] == ref[j];
			free(ord);
			qsort(idx, r, sizeof(int), int_cmp);
			qsort(ref, r, sizeof(int), int_cmp);
			for (j = 0; j < r; j++)
				ok &= idx[j] == ref[j] && (j == 0 || idx[j] > idx[j - 1]);
			memset(in, 0, sizeof(bool) * Max(n, 1));
			for (j = 0; j < r; j++)
				in[idx[j]] = true;
			for (j = 0; j < r; j++)
				for (i = 0; i < n; i++)
				{
					double		kj = isnan(key[idx[j]]) ? INFINITY : key[idx[j]];
					double		ki = isnan(key[i]) ? INFINITY : key[i];

					if (!in[i])
						ok &= kj < ki || (kj == ki && idx[j] < i);
				}
		}
		free(in);
		free(ref);
		free(idx);
		free(key);
	}
	CHECK(ok, "least: lionvec_rank()'s set, each kept key before every key left out");
	CHECK(okord, "least, ordered: lionvec_rank()'s order, ties by place, NaN last");
	{
		double		k2[6] = {3.0, NAN, 1.0, 3.0, -INFINITY, 1.0};
		int			i2[6] = {5, 3, 1, 0, 4, 2};

		lionvec_least_order(k2, i2, 6);
		CHECK(i2[0] == 4 && i2[1] == 2 && i2[2] == 5 && i2[3] == 0 && i2[4] == 3 &&
			  i2[5] == 1, "least, ordered: by key, ties by place, NaN last");
		lionvec_least_order(k2, i2, 0);
		lionvec_least_order(k2, i2, 1);
		CHECK(i2[0] == 4, "least, ordered: none and one left as they are");
	}
	CHECK(lionvec_least(NULL, 0, 4, NULL) == 0, "least of no keys");
	{
		double		k1[3] = {2.0, 1.0, 3.0};
		int			i1[1];

		CHECK(lionvec_least(k1, 3, 0, i1) == 0, "least of none wanted");
		CHECK(lionvec_least(k1, 3, 1, i1) == 1 && i1[0] == 1, "least of one wanted");
	}
}

/*
 * The locator's arithmetic (DESIGN.md §44.13): a vector's bytes in TOAST and
 * the chunks they take at 8K pages' 1,996 bytes - 2 at 768 dimensions and
 * halfvec(1536), 3 at 1024, 4 at 1536, 7 at 3072 - and each chunk's size;
 * the runs a REFINE item's slot holds; runs made from chunk places, merging
 * the consecutive line pointers of a block and nothing else, against a
 * model, refused past maxruns; a run list's checks; and the LOC pages
 * chosen for a scan's members, against a brute-force model over random
 * ranges, empty ones and repeated pages among them.
 */
static void
test_locator(void)
{
	bool		ok = true;
	int			trial;

	cur_phase = "locator";
	CHECK(lionvec_toast_extsize(1536, false) == 6148 &&
		  lionvec_toast_extsize(1536, true) == 3076 &&
		  lionvec_toast_nchunks(6148, 1996) == 4 &&
		  lionvec_toast_nchunks(lionvec_toast_extsize(768, false), 1996) == 2 &&
		  lionvec_toast_nchunks(lionvec_toast_extsize(1024, false), 1996) == 3 &&
		  lionvec_toast_nchunks(lionvec_toast_extsize(3072, false), 1996) == 7 &&
		  lionvec_toast_nchunks(lionvec_toast_extsize(1536, true), 1996) == 2 &&
		  lionvec_toast_nchunks(1996, 1996) == 1 &&
		  lionvec_toast_nchunks(1997, 1996) == 2 &&
		  lionvec_toast_nchunks(0, 1996) == 0,
		  "toast: a vector's bytes and chunks");
	CHECK(lionvec_toast_chunk_size(6148, 1996, 0) == 1996 &&
		  lionvec_toast_chunk_size(6148, 1996, 2) == 1996 &&
		  lionvec_toast_chunk_size(6148, 1996, 3) == 160 &&
		  lionvec_toast_chunk_size(3992, 1996, 1) == 1996 &&
		  lionvec_toast_chunk_size(100, 1996, 0) == 100,
		  "toast: each chunk's size, the last the rest");
	CHECK(lionvec_refloc_runs(1) == 1 && lionvec_refloc_runs(2) == 2 &&
		  lionvec_refloc_runs(3) == 2 && lionvec_refloc_runs(4) == 2 &&
		  lionvec_refloc_runs(7) == 3 && lionvec_refloc_runs(17) == 6,
		  "refine slot: runs by chunks");

	for (trial = 0; trial < 3000; trial++)
	{
		int			n = 1 + (int) (rnd64() % 20);
		int			maxruns = 1 + (int) (rnd64() % 22);
		uint32		blks[20];
		uint16		offs[20];
		LionVecLocRun runs[24];
		int			model = 0;
		int			r;
		int			i;
		int			seq;

		blks[0] = (uint32) (rnd64() % 4);
		offs[0] = (uint16) (1 + rnd64() % 6);
		for (i = 1; i < n; i++)
		{
			uint64		u = rnd64() % 4;

			/* the next line pointer, a gap, a new block, or the same block back */
			blks[i] = u == 2 ? blks[i - 1] + 1 : blks[i - 1];
			offs[i] = u == 0 ? offs[i - 1] + 1 : u == 1 ? offs[i - 1] + 2 :
				u == 2 ? 1 : offs[i - 1] - (offs[i - 1] > 1 ? 1 : 0);
		}
		for (i = 0; i < n; i++)
			if (i == 0 || blks[i] != blks[i - 1] || offs[i] != offs[i - 1] + 1)
				model++;
		r = lionvec_loc_runs(blks, offs, n, runs, maxruns);
		if (model > maxruns)
		{
			ok &= r == -1;
			continue;
		}
		ok &= r == model;
		if (r != model)
			continue;
		/* the chunks back, in order, from the runs */
		seq = 0;
		for (i = 0; i < r; i++)
		{
			int			j;

			for (j = 0; j < runs[i].n; j++, seq++)
				ok &= seq < n && runs[i].blk == blks[seq] && runs[i].off + j == offs[seq];
		}
		ok &= seq == n;
		ok &= lionvec_loc_runs_ok(runs, r, n, 291);
	}
	CHECK(ok, "runs: consecutive line pointers of a block merged, the chunks back in order");
	CHECK(lionvec_loc_runs(NULL, NULL, 0, NULL, 4) == -1, "runs: none of no chunks");
	{
		LionVecLocRun r1[2] = {{7, 1, 3}, {8, 1, 1}};
		LionVecLocRun bad[1] = {{7, 0, 4}};
		LionVecLocRun inv[1] = {{PG_UINT32_MAX, 1, 4}};
		LionVecLocRun far[1] = {{7, 290, 4}};

		CHECK(lionvec_loc_runs_ok(r1, 2, 4, 291) &&
			  !lionvec_loc_runs_ok(r1, 2, 5, 291) &&
			  !lionvec_loc_runs_ok(r1, 2, 3, 291) &&
			  !lionvec_loc_runs_ok(r1, 0, 4, 291) &&
			  !lionvec_loc_runs_ok(r1, 2, 1, 291) &&
			  !lionvec_loc_runs_ok(bad, 1, 4, 291) &&
			  !lionvec_loc_runs_ok(inv, 1, 4, 291) &&
			  !lionvec_loc_runs_ok(far, 1, 4, 291) &&
			  lionvec_loc_runs_ok(far, 1, 4, 293),
			  "runs: the chunks' count, line pointers and blocks checked");
	}

	ok = true;
	for (trial = 0; trial < 2000; trial++)
	{
		int			n = (int) (rnd64() % 40);
		int			nm = (int) (rnd64() % 30);
		uint32		page[40];
		uint32		lo[40];
		uint32		hi[40];
		uint32		mblks[30];
		uint32		out[40];
		bool		want[64];
		int			k;
		int			got;
		int			i;
		int			j;

		for (i = 0; i < n; i++)
		{
			page[i] = (uint32) (rnd64() % 64);
			lo[i] = (uint32) (rnd64() % 200);
			/* now and then an empty range, its lo past its hi */
			hi[i] = rnd64() % 5 == 0 && lo[i] > 0 ? lo[i] - 1 :
				lo[i] + (uint32) (rnd64() % 30);
			if (rnd64() % 9 == 0)
			{
				lo[i] = PG_UINT32_MAX;	/* an empty page's: no block yet */
				hi[i] = 0;
			}
		}
		k = 0;
		for (j = 0; j < nm; j++)
		{
			uint32		b = (uint32) (rnd64() % 230);

			mblks[k++] = b;
		}
		qsort(mblks, k, sizeof(uint32), int_cmp);
		for (i = j = 0; i < k; i++)
			if (j == 0 || mblks[j - 1] != mblks[i])
				mblks[j++] = mblks[i];
		nm = j;
		memset(want, 0, sizeof(want));
		for (i = 0; i < n; i++)
			for (j = 0; j < nm; j++)
				if (lo[i] <= hi[i] && mblks[j] >= lo[i] && mblks[j] <= hi[i])
					want[page[i]] = true;
		got = lionvec_loc_select(page, lo, hi, n, mblks, nm, out);
		k = 0;
		for (i = 0; i < 64; i++)
			if (want[i])
				ok &= k < got && out[k++] == (uint32) i;
		ok &= k == got;
	}
	CHECK(ok, "select: the pages whose ranges hold a member's block, in order, once");
}

/*
 * The exact path's page pricing (DESIGN.md §44.7, "Exact path pricing"):
 * lionvec_pages_in_order() is costsize.c's price of a bitmap heap scan's
 * pages - 875.23 for 966 rows of 834 pages at random_page_cost 4, the
 * Bitmap Heap Scan's cost less its index's, CPU and target in EXPLAIN on
 * 100,000 rows in TOAST; every page at seq_page_cost once the rows are
 * twice the pages, even two of two; a single page at random_page_cost;
 * never more pages than rows, nor than the relation has, at a price
 * between the two; no rows, or no pages, counted as one; and equal prices
 * give the pages.
 */
static void
test_costs(void)
{
	bool		ok = true;
	int			n;

	cur_phase = "costs";
	CHECK(fabs(lionvec_pages_in_order(966, 834, 4.0, 1.0) - 875.23) < 0.01,
		  "pages in order: a bitmap heap scan's price, to the cent");
	CHECK(lionvec_pages_in_order(2000, 1000, 4.0, 1.0) == 1000.0 &&
		  lionvec_pages_in_order(1e9, 1000, 4.0, 1.0) == 1000.0 &&
		  lionvec_pages_in_order(2000, 1000, 1.1, 1.0) == 1000.0,
		  "pages in order: every page, sequentially, at twice as many rows");
	CHECK(lionvec_pages_in_order(1, 1000, 4.0, 1.0) == 4.0 &&
		  lionvec_pages_in_order(1, 1, 4.0, 1.0) == 4.0 &&
		  lionvec_pages_in_order(0, 1000, 4.0, 1.0) == 4.0 &&
		  lionvec_pages_in_order(5, 0, 4.0, 1.0) == 4.0 &&
		  lionvec_pages_in_order(3, 2, 4.0, 1.0) == 2.0,
		  "pages in order: one row a random page, no rows one, a relation of none one page");
	for (n = 1; n <= 5000; n++)
	{
		double		T = 1.0 + (double) (n * 7919 % 3001);
		double		pages = lionvec_pages_in_order(n, T, 1.5, 1.5) / 1.5;
		double		c = lionvec_pages_in_order(n, T, 4.0, 1.0);

		ok &= fabs(pages - rint(pages)) < 1e-9 && pages >= 1.0 &&
			pages <= Min((double) n, T) &&
			c >= pages - 1e-9 && c <= 4.0 * pages + 1e-9;
	}
	CHECK(ok, "pages in order: no more pages than rows or than the relation's, priced between the two");
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
	test_halves();
	test_datums();
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
	/* last, so that the others' rows are what they were before these */
	test_subsample();
	test_candidates_among();
	test_regions();
	test_prefetch();
	test_locator();
	test_costs();
	test_partitions();

	printf("vec_test: %ld checks, %ld failed\n", nchecks, nfail);
	return nfail == 0 ? 0 : 1;
}
