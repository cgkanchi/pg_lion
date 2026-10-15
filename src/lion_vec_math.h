/*-------------------------------------------------------------------------
 * lion_vec_math.h
 *	  The arithmetic of lionvec (DESIGN.md §44.3 - §44.4): the rotation, the
 *	  bit-plane codes a vector is stored as, and the estimates, with their
 *	  error bounds, that a scan computes from those codes.
 *
 *	  Every vector is padded with zeros to a whole number of 64-bit words,
 *	  dimsp dimensions, rotated by a fixed pseudo-random rotation and centred
 *	  on the indexed rows' mean (also rotated), which leaves a vector x whose
 *	  coordinates look like independent Gaussians of variance |x|^2 / dimsp.
 *	  The codes of x are
 *
 *		plane 1		the sign of each coordinate: one bit a dimension, and
 *					alpha1 = <sign(x) / sqrt(dimsp), x / |x|>
 *		planes 2-8	with plane 1, an 8-bit code a dimension: x_i sqrt(dimsp) / |x|,
 *					a standard Gaussian, rounded down to a 32nd within four
 *					standard deviations; r is the vector of the steps'
 *					middles, and alpha8 = <r / |r|, x / |x|>
 *
 *	  and the inner product of x with a query q is estimated from either as
 *	  |x| <c, q> / (|c| alpha), c the code's vector; the error of that is
 *	  the part of x / |x| that c misses, sqrt(1 - alpha^2) long and pointing
 *	  nowhere in particular after the rotation, times q, which is Gaussian
 *	  with standard deviation |q| sqrt(1 - alpha^2) / (alpha sqrt(dimsp - 1))
 *	  (RaBitQ, Gao and Long, SIGMOD 2024).  A bound of epsilon of those
 *	  either side holds with the Gaussian's probability, whatever the data.
 *
 *	  Plane 1 is compared with a query quantized to four bits a dimension,
 *	  as four more bit-planes, so that the inner product is popcounts of
 *	  ANDs; the rounding adds Delta^2 / 12 to the variance, Delta the step.
 *
 *	  A vector comes from pgvector's `vector`, whose elements are floats, or
 *	  its `halfvec`, whose elements are IEEE 754 binary16 halves and are
 *	  converted to floats as they are read; everything after that is the same.
 *
 *	  Frontend-safe (c.h and libpgcommon only), so that test/unit/vec_test.c
 *	  can check it without a server.
 *-------------------------------------------------------------------------
 */
#ifndef LION_VEC_MATH_H
#define LION_VEC_MATH_H

#include <math.h>

/* Metrics, numbered as the operator classes' ORDER BY strategies. */
#define LIONVEC_L2				1	/* <-> */
#define LIONVEC_IP				2	/* <#> */
#define LIONVEC_COSINE			3	/* <=> */
#define LIONVEC_NSTRATEGIES		3

/* The rotation: rounds of random signs and overlapping Hadamard blocks. */
#define LIONVEC_ROUNDS			3

/* The query's quantization: bits a dimension, so planes a word of plane 1. */
#define LIONVEC_QBITS			4

/*
 * The 8-bit codes' bound is twice as wide, in standard deviations, as plane
 * 1's: its deviation is about 1/70 of plane 1's, so twice as wide is still
 * far the tighter, and its failures - the rows a scan finds too late - all
 * but vanish.
 */
#define LIONVEC_EPS8(eps)		(2.0 * (eps))

/* dims padded to whole 64-bit words */
#define LIONVEC_PADDED(dims)	((((dims) + 63) / 64) * 64)

/*
 * pgvector's `vector` and `halfvec`, as they lay their datums out: a
 * varlena of a 16-bit dimension count, 16 unused bits and the elements,
 * floats or halves (IEEE 754 binary16, which lionvec_halves() converts).
 * pg_lion reads them here rather than linking pgvector, so that pg_lion
 * itself loads without pgvector.
 */
typedef struct LionVecPgVector
{
	int32		vl_len_;
	int16		dim;
	int16		unused;
	float		x[FLEXIBLE_ARRAY_MEMBER];
} LionVecPgVector;

typedef struct LionVecPgHalfVector
{
	int32		vl_len_;
	int16		dim;
	int16		unused;
	uint16		x[FLEXIBLE_ARRAY_MEMBER];
} LionVecPgHalfVector;

/*
 * The dimension count of a detoasted `vector` datum, or of a `halfvec` one
 * if half, of len bytes at p, its varlena header included; or -1 if those
 * bytes cannot be one: fewer than the header, a negative count, or fewer
 * than the count's elements take.
 */
extern int	lionvec_datum_dim(const void *p, Size len, bool half);

typedef struct LionVecRotation
{
	int			dimsp;
	int			nwin;			/* Hadamard windows: 1, or 2 overlapping */
	int			block;			/* their size, a power of two */
	int			win[2];			/* their starts */
	float	   *signs;			/* [LIONVEC_ROUNDS][nwin][dimsp], +-1 */
} LionVecRotation;

/* What a row's codes are, before they are laid out in its items. */
typedef struct LionVecEncoded
{
	float4		norm;			/* |x|, x centred */
	float4		alpha1;
	float4		alpha8;
	float4		rnorm8;			/* |r| */
	float4		xm;				/* <x, mean> */
	uint16		popcount;		/* set bits of plane 1 */
	uint64	   *bits;			/* [dimsp / 64], plane 1 */
	uint8	   *codes;			/* [dimsp], 8-bit codes */
} LionVecEncoded;

/*
 * A query, prepared against one index: centred and rotated as the rows are,
 * and quantized for plane 1.
 */
typedef struct LionVecQuery
{
	int			dimsp;
	float	   *qc;				/* [dimsp], q - mean, rotated */
	uint64	   *planes;			/* [dimsp / 64][LIONVEC_QBITS], interleaved */
	double		qlo;			/* the quantization: qc_i ~ qlo + qdelta * u_i */
	double		qdelta;
	double		qsum;			/* sum of the quantized qc_i */
	double		qnorm2;			/* |qc|^2 */
	double		var1;			/* |qc|^2 / (dimsp - 1): the codes' variance factor */
	double		varq;			/* the quantization's own variance, plane 1 only */
	double		eps;			/* the bounds' width, in standard deviations */
	double		offset;			/* key minus distance: lionvec_key_exact() */
} LionVecQuery;

/* the 8-bit codes: 256 steps of 1/32 over [-4, 4), and their middles */
#define LIONVEC_CLIP8			4.0f
#define LIONVEC_STEPS8			32.0f
extern float lionvec_recon8[256];

extern void lionvec_tables_init(void);
extern uint8 lionvec_code8(float z);

extern Size lionvec_rotation_nsigns(int dimsp);
extern void lionvec_rotation_init(LionVecRotation *rot, int dimsp, uint32 seed,
								  float *signs);
extern void lionvec_rotate(const LionVecRotation *rot, float *x);

extern bool lionvec_prepare(const LionVecRotation *rot, int metric,
							const float *v, int dims, float *x);
extern void lionvec_encode(const float *mean, int dimsp, float *x,
						   LionVecEncoded *e);

extern void lionvec_query_init(LionVecQuery *q, int metric, const float *mean,
							   const float *x, double eps);

/*
 * The kernels (DESIGN.md §44.3), chosen once by lionvec_kernels_init():
 *
 *	lionvec_plane_dot	sum over j of 2^j popcount(bits & plane j), the inner
 *						product of a row's sign bits with the quantized
 *						query's codes, over nwords words of each
 *	lionvec_sum8		sum over i < dimsp of lionvec_recon8[codes[i]] qc[i]
 *	lionvec_l2sq		sum over i < dimsp of (a[i] - b[i])^2
 *	lionvec_halves		x[i] = the float of the half h[i], i < n: a halfvec's
 *						elements, exactly, a NaN as the quiet NaN of its
 *						payload
 *
 * and the rotation's, inside lionvec_rotate().  The sums are in double, in
 * LIONVEC_LANES partial sums - the i-th term into partial i % LIONVEC_LANES
 * - added up in a fixed order, which every kernel keeps: each returns what
 * the portable one does, to the bit, as do the rotation's and the halves'
 * (lion_vec_math.c, "Kernels").  dimsp is a multiple of 64.
 */
#define LIONVEC_LANES			16

typedef uint32 (*LionVecDotKernel) (const uint64 *bits, const uint64 *planes,
									int nwords);
typedef double (*LionVecSum8Kernel) (const uint8 *codes, const float *qc,
									 int dimsp);
typedef double (*LionVecL2Kernel) (const float *a, const float *b, int dimsp);
typedef void (*LionVecHalfKernel) (const uint16 *h, int n, float *x);
extern LionVecDotKernel lionvec_plane_dot;
extern LionVecSum8Kernel lionvec_sum8;
extern LionVecL2Kernel lionvec_l2sq;
extern LionVecHalfKernel lionvec_halves;
extern void lionvec_kernels_init(void);

/* the sets of kernels, as the unit test forces them */
#define LIONVEC_KERNELS_PORTABLE	0
#define LIONVEC_KERNELS_POPCNT		1	/* x86-64: POPCNT for plane 1 */
#define LIONVEC_KERNELS_AVX2		2	/* x86-64: AVX2 for all; halves by
										 * F16C where the CPU has it */
#define LIONVEC_KERNELS_AVX512		3	/* x86-64: AVX2, plane 1 by VPOPCNTDQ */
#define LIONVEC_KERNELS_MAX			LIONVEC_KERNELS_AVX512
#ifdef FRONTEND
/* the unit test's way to run each set; false if the build or the CPU lacks it */
extern bool lionvec_kernel_force(int which);
/* ... and whether the set it forced converts halves with F16C */
extern bool lionvec_kernel_f16c(void);
#endif

extern void lionvec_estimate1(const LionVecQuery *q, const uint64 *bits,
							  uint16 popcount, float norm, float alpha1,
							  double *est, double *err);
extern void lionvec_estimate8(const LionVecQuery *q, const uint8 *codes,
							  float norm, float alpha8, float rnorm8,
							  double *est, double *err);

/*
 * Partitions (DESIGN.md §44.2): k centroids, each coded as a row is, and the
 * k-means that finds them.  A row belongs to the centroid nearest it by L2;
 * cosine's centroids are of unit length, as its rows are, so that nearest by
 * L2 is nearest by angle too.
 */
/* the width of the bounds a row's nearest centroids are found by */
#define LIONVEC_ASSIGN_EPS		4.0

typedef struct LionVecCentroids
{
	int			k;
	int			dimsp;
	const uint64 *bits;			/* [k][dimsp / 64]: plane 1 */
	const uint16 *popcount;		/* [k] */
	const float *norm;			/* [k]: |c|, c centred */
	const float *alpha1;		/* [k] */
} LionVecCentroids;

/*
 * The centroids that can be the nearest to the row the query q was made from
 * (lionvec_query_init(), LIONVEC_L2): each one's key bounds into lo[k] and
 * hi[k], and into cand[k] the indices of those whose lower bound is no more
 * than the least upper bound, which it returns the number of - at least one
 * when k is.  The bounds' width is q's.
 */
extern int	lionvec_candidates(const LionVecCentroids *c, const LionVecQuery *q,
							   double *lo, double *hi, int *cand);

/*
 * ... and the centroids that can be among the m nearest (1 <= m <=
 * LIONVEC_RANK_MAX), those whose lower bound is no more than the m-th least
 * upper bound: at least min(m, k) of them, as every centroid's lo <= hi, or
 * its bound is none - which holds when each norm is a non-negative number,
 * and which callers reading centroids from an index check rather than
 * assume.  m = 1 is lionvec_candidates().
 */
#define LIONVEC_RANK_MAX		16
extern int	lionvec_candidates_n(const LionVecCentroids *c, const LionVecQuery *q,
								 int m, double *lo, double *hi, int *cand);

/*
 * The m least of key[0 .. n), as their places in key, least first: into
 * order[], which must hold min(n, m) of them, and how many that is.  Ties go
 * to the lower place, and a NaN key counts as infinite.
 */
extern int	lionvec_rank(const double *key, int n, int m, int *order);

/*
 * Spill (DESIGN.md §44.3): an index that spills keeps every row with a
 * vector in two partitions, its nearest centroid c1's and a second, chosen
 * by SOAR (Sun and others, NeurIPS 2023): of the LIONVEC_SOAR_CANDS
 * centroids next nearest after c1, the one that minimizes
 *
 *		|x - c|^2 + lambda <r / |r|, x - c>^2,		r = x - c1
 *
 * - a centroid near the row, and off the line of its residual r, so that a
 * query whose nearest rows are the row lies where c1's partition was likely
 * to be passed by (where the query lies along r beyond the row) finds the
 * row in the second instead.  lambda = 0 is the plain second-nearest.
 * lionvec_soar_cost() is that cost of a centroid c, x, c1 and c dimsp
 * floats alike centred; lionvec_soar_second() the place in cands[0 .. n) of
 * the one of least cost, ties to the lower place (the nearer, when cands are
 * nearest first), or -1 when n is 0.  A row at c1 has no residual and costs
 * |x - c|^2; a cost that is not a number counts as infinite.
 */
#define LIONVEC_SOAR_CANDS		8
#define LIONVEC_SOAR_LAMBDA		1.0
extern double lionvec_soar_cost(const float *x, const float *c1, const float *c,
								int dimsp, double lambda);
extern int	lionvec_soar_second(const float *x, const float *c1,
								const float *const *cands, int n, int dimsp,
								double lambda);

/*
 * A vector from its 8-bit codes, norm (|x|), alpha8 and rnorm8 (|r|), as an
 * index stores a centroid's: x's projection on r, |x| alpha8 r / |r|, the
 * nearest vector to x in r's direction, |x| sqrt(1 - alpha8^2) from it.
 * Zeros for codes with no length.
 */
extern void lionvec_decode8(const uint8 *codes, int dimsp, float norm,
							float alpha8, float rnorm8, float *x);

/*
 * A sample of the rows, for k-means: each one rotated, not centred (the
 * mean is not known when the sample is taken), and held as its 8-bit codes,
 * so that a sample of 1536 dimensions takes 1.5 KB a row, not 6.  Row i is
 * scale[i] * lionvec_recon8[codes[i * dimsp + j]], j < dimsp.
 */
typedef struct LionVecSample
{
	int			n;
	int			dimsp;
	const uint8 *codes;			/* [n][dimsp] */
	const float *scale;			/* [n]: |x| / |r| */
} LionVecSample;

extern void lionvec_sample_row(const LionVecSample *s, int i, float *x);

/*
 * What lionvec_kmeans() works in: the caller's arrays, of the lengths given,
 * so that it allocates nothing.
 */
typedef struct LionVecKmeansWork
{
	int		   *count;			/* [k] */
	int		   *assign;			/* [n] */
	uint64	   *bits;			/* [k][dimsp / 64] */
	uint16	   *popcount;		/* [k] */
	float	   *norm;			/* [k] */
	float	   *alpha1;			/* [k] */
	double	   *lo;				/* [k] */
	double	   *hi;				/* [k] */
	int		   *cand;			/* [k] */
	float	   *x;				/* [dimsp] */
	float	   *y;				/* [dimsp] */
	uint8	   *codes;			/* [dimsp] */
	float	   *qc;				/* [dimsp] */
	uint64	   *planes;			/* [dimsp / 64][LIONVEC_QBITS] */
} LionVecKmeansWork;

/*
 * Lloyd's k-means of the sample's n rows into k <= n centroids cent[k][dimsp]
 * (rotated, not centred), over iters iterations from k distinct rows the
 * seed picks; unit, the centroids are scaled to unit length after each
 * (spherical k-means, for cosine).  Each row goes to its nearest centroid by
 * L2, found among lionvec_candidates() by the exact distance; a centroid
 * left with no rows takes half of the largest's (FAISS's way).  mean, the
 * rows' (rotated; NULL: zero), centres the codes the candidates are found
 * by, which only makes their bounds tighter.  Returns the iterations made:
 * fewer than iters once an iteration moves no row.
 */
extern int	lionvec_kmeans(const LionVecSample *s, int k, int iters, bool unit,
						   uint64 seed, const float *mean, float *cent,
						   LionVecKmeansWork *w);

/*
 * The scan orders rows by a KEY that is the distance up to a constant of the
 * query: what changes from row to row of an estimate of <x, qc>.
 *
 *	L2		|x - q|^2 = |x|^2 - 2 <x, qc> + |qc|^2		key = |x|^2 - 2 <x, qc>
 *	IP		-<x + m, q> = -<m, q> - <x, m> - <x, qc>	key = -<x, m> - <x, qc>
 *	cosine	the same, of q / |q| and of the rows' unit vectors, minus 1
 *
 * x the centred row, m the mean.  lionvec_key_bounds() gives the key's
 * bounds from an estimate of <x, qc> and its error, and lionvec_key_exact()
 * the key of a distance computed exactly, given the query's offset
 * (lionvec_query_init()): |qc|^2 for L2, <m, q> for IP, <m, q> - 1 for
 * cosine.  A bound that is not a number - a vector so large its norm
 * overflowed - is no bound at all, and is returned as an infinite one.
 */
static inline void
lionvec_key_bounds(int metric, double norm, double xm, double est, double err,
				   double *lo, double *hi)
{
	if (metric == LIONVEC_L2)
	{
		double		base = norm * norm;

		*lo = base - 2.0 * (est + err);
		*hi = base - 2.0 * (est - err);
	}
	else
	{
		*lo = -xm - est - err;
		*hi = -xm - est + err;
	}
	if (isnan(*lo) || isnan(*hi))
	{
		*lo = -INFINITY;
		*hi = INFINITY;
	}
}

static inline double
lionvec_key_exact(int metric, double dist, double offset)
{
	return metric == LIONVEC_L2 ? dist * dist - offset : dist + offset;
}

/*
 * An inner product's lower bound on the keys of a partition's rows, from a
 * lower bound lo on its centroid c's key, as a scan ranks the partitions
 * (DESIGN.md §44.4).  Each row x is lifted by one more coordinate,
 * sqrt(maxnorm2 - |x|^2), maxnorm2 the partition's greatest |x|^2: the rows
 * then lie at one distance from the origin, and the nearer one lies to the
 * unit query (q / |q|, 0), the greater its <q, x> (Bachrach and others,
 * RecSys 2014).  The rows' centroid lies inside that sphere by spread, their
 * mean |x - c|^2, at squared distance 1 + maxnorm2 - spread - 2 <q / |q|, c>
 * from the query; the bound is the key of a row margin nearer than that, so
 * a margin as large as the lifted rows' greatest distance from their
 * centroid makes it a true bound.  offset and qnorm are the query's offset
 * and |q|; a zero query, whose every key is the same, gets lo back.
 */
static inline double
lionvec_lift_key(double lo, double offset, double qnorm, double maxnorm2,
				 double spread, double margin)
{
	double		qc;
	double		d;

	if (isinf(lo) || !(qnorm > 0.0))
		return lo;
	qc = -(lo - offset) / qnorm;	/* <q / |q|, c>, at most */
	d = sqrt(Max(1.0 + maxnorm2 - spread - 2.0 * qc, 0.0)) - margin;
	if (d < 0.0)
		d = 0.0;
	return qnorm * (d * d - 1.0 - maxnorm2) / 2.0 + offset;
}

#endif							/* LION_VEC_MATH_H */
