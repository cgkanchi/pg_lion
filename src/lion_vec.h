/*-------------------------------------------------------------------------
 *
 * lion_vec.h
 *		lionvec: a nearest-neighbour index access method for pgvector's
 *		`vector` and `halfvec` types, shipped beside lion (DESIGN.md
 *		section 44).
 *
 *		Every vector is rotated by a fixed random rotation, centred on the
 *		table's mean and quantized into eight nested bit-planes.  The rows
 *		are partitioned by k-means, and a scan reads the partitions whose
 *		centroids are nearest the query, nearest first, until the next
 *		partition's rows are taken to lie beyond the rows it has.  The first
 *		plane (one bit per dimension) is all a scan reads of a partition's
 *		rows; it gives each row's distance to the query with an error bound
 *		that holds with a chosen probability whatever the data, so only the
 *		rows that bound cannot rule out are refined from the other seven
 *		planes, and only the few those cannot order are compared exactly,
 *		from the heap.
 *
 *		The index is created by the pg_lion_vector extension, which needs
 *		pgvector; this library reads pgvector's on-disk format itself and
 *		calls its distance functions only through the operator class, so
 *		pg_lion loads without pgvector installed.
 *
 *		lion_vec.c holds the format, the build, inserts, VACUUM, costs and
 *		the handler; lion_vec_scan.c the scan; lion_vec_math.c (behind
 *		lion_vec_math.h, which the unit test builds without a server) the
 *		rotation, the codes and the bounds.  The rest of pg_lion sees only
 *		the functions at the end: LionOrdered (lion_ordered.c) uses them to
 *		filter a lionvec scan by lion's exact TID set (§44.6).
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_VEC_H
#define LION_VEC_H

#include "access/genam.h"
#include "access/relscan.h"
#include "catalog/pg_type_d.h"
#include "storage/bufmgr.h"
#include "nodes/pathnodes.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"
#include "varatt.h"

#include "lion_container.h"
#include "lion_vec_math.h"

/* ---------------------------------------------------------------------
 * Format (DESIGN.md §44.2)
 * --------------------------------------------------------------------- */

#define LIONVEC_MAGIC			0x4c56454d	/* "LVEM" */
#define LIONVEC_VERSION			5
#define LIONVEC_METAPAGE		0

/* The rotation's seed.  Kept in the meta page, so it can change later. */
#define LIONVEC_SEED			0x6c696f6e	/* "lion" */

/* Page types, in the special space of every page. */
#define LIONVEC_PAGE_META		0
#define LIONVEC_PAGE_MEAN		1
#define LIONVEC_PAGE_CODE		2
#define LIONVEC_PAGE_REFINE		3
#define LIONVEC_PAGE_PART		4
#define LIONVEC_PAGE_REGION		5
#define LIONVEC_PAGE_LOC		6	/* the locator's entries (§44.13) */
#define LIONVEC_PAGE_LOCDIR		7	/* ... and its directory */

/* Identifies a lionvec page beside other AMs' (as GIN's and GiST's do). */
#define LIONVEC_PAGE_ID			0xFF8C

typedef struct LionVecPageOpaqueData
{
	BlockNumber next;			/* the next page of this chain */
	uint32		minkey;			/* CODE: least container key of its rows;
								 * LOC: least heap block of its entries;
								 * LOCDIR: least of its items' ranges */
	uint32		maxkey;			/* ... greatest */
	uint16		type;			/* LIONVEC_PAGE_* */
	uint16		page_id;		/* LIONVEC_PAGE_ID */
} LionVecPageOpaqueData;

typedef LionVecPageOpaqueData *LionVecPageOpaque;

#define LionVecPageGetOpaque(page) \
	((LionVecPageOpaque) PageGetSpecialPointer(page))

/* LionVecMetaData's flags */
#define LIONVEC_META_SPILL		0x0001	/* rows spill to a second partition */
#define LIONVEC_META_LOCATOR	0x0002	/* it keeps a locator (§44.13) */

typedef struct LionVecMetaData
{
	uint32		magic;
	uint32		version;
	uint16		metric;			/* LIONVEC_L2 / _IP / _COSINE */
	uint16		flags;			/* LIONVEC_META_*: as built */
	int32		dims;			/* 0: no vector seen yet */
	int32		dimsp;			/* dims padded to a multiple of 64 */
	uint32		seed;			/* the rotation's */
	BlockNumber mean_head;		/* InvalidBlockNumber: the mean is zero */
	BlockNumber part_head;		/* the PART chain, or InvalidBlockNumber */
	uint32		nparts;			/* the partitions it holds */
	BlockNumber rest_head;		/* the CODE chain of rows in no partition */
	BlockNumber rest_tail;
	BlockNumber null_head;		/* the CODE chain of NULL rows and cosine zeros */
	BlockNumber null_tail;
	BlockNumber refine_tail;	/* where the next REFINE item goes */
	float4		scale;			/* the rows' standard deviation about their
								 * centroids along one dimension, as built: the
								 * unit of pg_lion.vector_margin */
	double		ntuples;		/* rows as of the build */
	BlockNumber region_head;	/* the REGION chain, or InvalidBlockNumber */
	uint32		nregions;		/* the regions it holds; 0: none (§44.3) */

	/*
	 * The locator (DESIGN.md §44.13), LIONVEC_META_LOCATOR; format 5 on.  An
	 * index without one has its blocks invalid and the rest 0.
	 */
	BlockNumber loc_head;		/* the LOCDIR chain */
	BlockNumber loc_dtail;		/* ... its last page */
	BlockNumber loc_tail;		/* the LOC page inserts append to */
	Oid			toastrelid;		/* the heap's TOAST relation, as built */
	uint32		loc_built;		/* LOCDIR items the build wrote, in heap
								 * order, from loc_head on, filling its
								 * pages: the static level */
	uint32		loc_bpages;		/* ... the LOCDIR pages they fill */
	BlockNumber loc_root;		/* a level over the static one: invalid,
								 * and not read (§44.13, "A second level") */
	uint16		refloc_runs;	/* runs a REFINE item's place holds; 0: its
								 * items have none */
	uint16		loc_pad;
} LionVecMetaData;

typedef LionVecMetaData *LionVecMeta;

#define LionVecPageGetMeta(page) ((LionVecMeta) PageGetContents(page))

/*
 * A row's first plane: what every scan reads.  NULL rows and the zero vectors
 * of a cosine index carry the header alone.  In an index that spills
 * (LIONVEC_META_SPILL), every other row has a second item, its copy, in a
 * second partition's chain: the same in every field, the REFINE item's place
 * included, but for LIONVEC_SPILL (DESIGN.md §44.3).
 */
#define LIONVEC_NULL			0x0001	/* the column is NULL */
#define LIONVEC_ZERO			0x0002	/* cosine: a zero vector, distance NaN */
#define LIONVEC_SPILL			0x0004	/* a row's copy in its second partition */

typedef struct LionVecCode
{
	ItemPointerData tid;
	uint16		flags;
	float4		norm;			/* |x|, x the row padded, rotated and centred */
	float4		alpha1;			/* <sign(x) / sqrt(dimsp), x / |x|> */
	float4		xm;				/* <x, mean>: the IP and cosine keys' */
	BlockNumber rblk;			/* the REFINE item */
	OffsetNumber roff;
	uint16		popcount;		/* set bits of the plane */
	uint64		bits[FLEXIBLE_ARRAY_MEMBER];	/* dimsp / 64 words */
} LionVecCode;

#define LIONVEC_CODE_HDRSZ		offsetof(LionVecCode, bits)
#define LIONVEC_CODE_SIZE(dimsp) (LIONVEC_CODE_HDRSZ + (Size) (dimsp) / 8)

/*
 * ... and the rest of its planes, read only for the rows plane 1 keeps.  In
 * an index whose meta page gives refloc_runs, every REFINE item ends with
 * a slot for where its row's vector lies in TOAST (LionVecRefineLoc), so
 * that a scan reranking the row reads it there (§44.13): its value's id and
 * up to refloc_runs runs, nloc of them in use - 0 for a centroid's, or a
 * row whose vector has no place.
 */
typedef struct LionVecRefine
{
	ItemPointerData tid;		/* the row's, to tell a reused slot (§44.5) */
	uint16		nloc;			/* runs in its place; 0: none (0 before
								 * format 5, a pad) */
	float4		alpha8;			/* <r / |r|, x / |x|>, r the codes' vector */
	float4		rnorm8;			/* |r| */
	uint8		codes[FLEXIBLE_ARRAY_MEMBER];	/* dimsp 8-bit codes */
} LionVecRefine;

#define LIONVEC_REFINE_SIZE(dimsp) (offsetof(LionVecRefine, codes) + (Size) (dimsp))

typedef struct LionVecRefineLoc
{
	Oid			valueid;		/* va_valueid of the row's TOAST pointer */
	LionVecLocRun runs[FLEXIBLE_ARRAY_MEMBER];	/* refloc_runs, nloc in use */
} LionVecRefineLoc;

#define LIONVEC_REFLOC_SIZE(nr) \
	(offsetof(LionVecRefineLoc, runs) + (Size) (nr) * sizeof(LionVecLocRun))
/* a REFINE item's size, with its slot when its index has them */
#define LIONVEC_REFINE_ITEM_SIZE(dimsp, nr) \
	(LIONVEC_REFINE_SIZE(dimsp) + ((nr) > 0 ? LIONVEC_REFLOC_SIZE(nr) : 0))
#define LionVecRefineGetLoc(ref, dimsp) \
	((LionVecRefineLoc *) ((ref)->codes + (dimsp)))

/*
 * The locator (DESIGN.md §44.13): for each row whose vector lies in TOAST
 * as a value the index's dimensions give the length of, uncompressed, an
 * entry on a LOC page - the row's TID, its value's id and where its chunks
 * lie, as runs (lion_vec_math.h) - so that the exact path reads members'
 * vectors from their chunks' pages directly.  The build's LOC pages hold
 * its rows in heap order, in one run of blocks; inserts append to the tail,
 * a page the build leaves empty.  The LOCDIR chain has an item a LOC page:
 * its block and the least and greatest heap block of the entries it has
 * held, which VACUUM never narrows.  The build's items come first, in heap
 * order, filling its LOCDIR pages from loc_head (loc_built of them, the
 * static level); the tail's and the inserts' pages' follow.  Every place
 * is a hint, checked chunk by chunk before it is used.
 */
typedef struct LionVecLoc
{
	ItemPointerData tid;		/* the row's */
	uint8		nruns;			/* 1 to the value's chunks */
	uint8		flags;			/* 0 (a later format's) */
	Oid			valueid;		/* va_valueid of its TOAST pointer */
	LionVecLocRun runs[FLEXIBLE_ARRAY_MEMBER];
} LionVecLoc;

#define LIONVEC_LOC_SIZE(nruns) \
	(offsetof(LionVecLoc, runs) + (Size) (nruns) * sizeof(LionVecLocRun))

typedef struct LionVecLocDir
{
	BlockNumber blk;			/* a LOC page */
	BlockNumber lo;				/* the least heap block of its entries, ever:
								 * InvalidBlockNumber while it has had none */
	BlockNumber hi;				/* ... and the greatest; 0 while none */
} LionVecLocDir;

/*
 * A partition (DESIGN.md §44.2), an item of the PART chain: where its CODE
 * chain starts and ends, how many items the build put in it (its rows, and
 * other rows' copies), how long the longest of their rows is and how far
 * they lie from the centroid (an inner-product scan's bound, §44.4), and the
 * centroid itself, coded as a row is - its plane 1 here and its 8-bit codes
 * in a REFINE item, whose TID is (partno, 0), which no heap row's is.  The
 * build writes the PART chain last; an insert changes only a partition's
 * tail.
 */
typedef struct LionVecPart
{
	uint32		partno;			/* its place in the chain, from 0 */
	BlockNumber head;			/* its CODE chain */
	BlockNumber tail;			/* ... whose last page inserts append to */
	uint32		nrows;			/* items the build put in it, copies included */
	float4		maxnorm;		/* the largest |x| of their rows */
	float4		spread;			/* ... and the mean of |x - c|^2, c the centroid */
	LionVecCode cent;			/* the centroid's plane 1, as a row's */
} LionVecPart;

#define LIONVEC_PART_SIZE(dimsp) \
	(offsetof(LionVecPart, cent) + LIONVEC_CODE_SIZE(dimsp))

/*
 * A region (DESIGN.md §44.2, §44.3), an item of the REGION chain of an index
 * of LIONVEC_REGION_MIN partitions or more: the partitions first .. first +
 * nparts - 1, whose PART items follow one another along the PART chain from
 * (pblk, poff), and their centroids' centroid, coded as a row is - plane 1
 * alone, its TID and REFINE item invalid.  An insert looks for a row's
 * partition among those of the regions nearest it (lionvec_route()), as the
 * build did.  A region with no partitions has no PART item to point at.
 * The build writes the REGION chain after the PART chain, and nothing
 * changes it after.
 */
typedef struct LionVecRegion
{
	uint32		regno;			/* its place in the chain, from 0 */
	uint32		first;			/* its first partition */
	uint32		nparts;			/* ... and how many, in order */
	BlockNumber pblk;			/* the first one's PART item, or invalid */
	OffsetNumber poff;
	uint16		pad1;
	uint32		pad2;
	LionVecCode cent;			/* the centroids' centroid, plane 1 */
} LionVecRegion;

#define LIONVEC_REGION_SIZE(dimsp) \
	(offsetof(LionVecRegion, cent) + LIONVEC_CODE_SIZE(dimsp))

/* the TID of a centroid's REFINE item */
#define LionVecCentTid(tid, partno) \
	ItemPointerSet((tid), (BlockNumber) (partno), InvalidOffsetNumber)
/* ... and back: an invalid TID, which the checked accessors assert against */
#define LionVecCentPartno(tid)	ItemPointerGetBlockNumberNoCheck(tid)

/*
 * The most dimensions an index takes: as many whole words as leave a REFINE
 * item, its line pointer and the special space room on a page (8128 at 8K).
 * pgvector's `vector` goes to 16000, its HNSW and IVFFlat indexes to 2000.
 */
#define LIONVEC_MAX_DIM \
	((int) ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)) - \
			 MAXALIGN(sizeof(LionVecPageOpaqueData)) - \
			 MAXALIGN(offsetof(LionVecRefine, codes))) / 64 * 64))

/* ---------------------------------------------------------------------
 * What a backend keeps per index (rd_amcache) and per encoded vector
 * --------------------------------------------------------------------- */

typedef struct LionVecInfo
{
	int			metric;
	bool		half;			/* the column is a halfvec, not a vector */
	int			dims;			/* 0: no vector indexed yet */
	int			dimsp;
	uint32		seed;
	LionVecRotation rot;		/* its signs follow this struct */
	float	   *mean;			/* [dimsp], rotated; NULL when zero */
	bool		locator;		/* LIONVEC_META_LOCATOR (§44.13) */
	int			reflocruns;		/* the meta page's refloc_runs */
	Oid			toastrelid;		/* ... and toastrelid */
} LionVecInfo;

/* the column's type, as pgvector's errors name it */
static inline const char *
lionvec_type_name(bool half)
{
	return half ? "halfvec" : "vector";
}

/*
 * The container key of a heap TID, as lion's sets are keyed (lion_tid.h),
 * from its block alone: what a CODE page's minkey and maxkey hold.
 */
static inline uint32
lionvec_ckey(ItemPointer tid)
{
	return (uint32) (ItemPointerGetBlockNumber(tid) >>
					 (LION_CONTAINER_BITS - LION_OFFSET_BITS));
}

/*
 * One more page of a chain: a chain longer than the relation has pages has
 * a cycle in it, which would otherwise be walked forever.  The relation may
 * have grown since *nblocks was read, so it is read again before saying so.
 */
static inline void
lionvec_chain_step(Relation index, uint64 *visited, BlockNumber *nblocks)
{
	if (++(*visited) > *nblocks)
	{
		*nblocks = RelationGetNumberOfBlocks(index);
		if (*visited > *nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a cycle in a page chain",
							RelationGetRelationName(index))));
	}
}

/*
 * Settings.  The margin is in standard deviations of the rows about their
 * centroids (LionVecMetaData's scale; lion_vec_scan.c, lv_margin()).
 */
#define LIONVEC_MARGIN_DEFAULT	3.0
/*
 * A scan stops probing once it has scored this many units of rows without
 * one entering its batch's L least upper bounds, until it has returned the
 * rows it holds; a unit is sqrt(n) / 2 rows, n the build's, the rows an
 * average partition would hold in 2 sqrt(n) partitions, or that share of
 * the members under a restriction (lion_vec_scan.c, lv_patience_unit();
 * DESIGN.md §44.4).  0 never stops.
 */
#define LIONVEC_PATIENCE_DEFAULT 30

/*
 * A scan restricted to this many members or fewer computes every member's
 * distance from the heap rather than probe partitions (DESIGN.md §44.6):
 * pg_lion.vector_exact_rows's default, the floor of lionvec_exact_limit().
 */
#define LIONVEC_EXACT_DEFAULT	2000

/*
 * ... and so does one restricted to no more than this many members a
 * partition, on average, if that is more: how thinly the members spread
 * over the partitions is what decides between the two, a probe reading a
 * partition's CODE pages for the few members among them (lion_vec.c,
 * lionvec_exact_limit(); DESIGN.md §44.6).  The default of
 * pg_lion.vector_exact_per_partition, and the most it may be, a bound only.
 */
#define LIONVEC_EXACT_PER_PART_DEFAULT	1.5
#define LIONVEC_EXACT_PER_PART_MAX		1000000.0

/*
 * What a scan does when what it knows of its rows outgrows hash_mem
 * (lion_vec_scan.c, lv_seen_stop(); DESIGN.md §44.4):
 * pg_lion.vector_overflow's values.
 */
typedef enum LionVecOverflow
{
	LIONVEC_OVERFLOW_ERROR,		/* fail the query (the default) */
	LIONVEC_OVERFLOW_STOP		/* return no more rows, with a NOTICE */
} LionVecOverflow;

/*
 * What the planner does with a lionvec scan that a Filter would follow
 * (lion_ordered.c, lion_ordered_set_rel_pathlist(); DESIGN.md §44.6,
 * "Post-filtering"): pg_lion.vector_postfilter's values.  The scan's pause
 * and its order frontier move on the rows it returns, and a Filter above it
 * rejects most of those, so such a plan finds other rows than LionOrdered,
 * whose scan returns members only, at the same margin and patience.
 */
typedef enum LionVecPostfilter
{
	LIONVEC_POSTFILTER_EXACT,	/* no such plan: LionOrdered, or an exact one */
	LIONVEC_POSTFILTER_ERROR,	/* ... and a query left without LionOrdered
								 * fails at plan time (the default) */
	LIONVEC_POSTFILTER_ALLOW	/* the plain scan and its Filter compete on
								 * cost, as before 2026-10-16 */
} LionVecPostfilter;

/*
 * What a build does when its sample, what half of maintenance_work_mem
 * holds, has rows for fewer partitions than the table's rows want
 * (lion_vec_math.h, lionvec_parts_short(); lion_vec.c,
 * lionvec_build_report_short(); DESIGN.md §44.8):
 * pg_lion.vector_build_sample's values.
 */
typedef enum LionVecBuildSample
{
	LIONVEC_BUILD_SAMPLE_ERROR,	/* fail the build (the default) */
	LIONVEC_BUILD_SAMPLE_ALLOW	/* make the partitions the sample has rows
								 * for, with a NOTICE */
} LionVecBuildSample;

/*
 * The most pages a scan asks for ahead of its reads, at each place it reads
 * from, whatever effective_io_concurrency says (lion_vec_scan.c,
 * lv_start_prefetch(); DESIGN.md §44.4, "Prefetch"): at 200 a look ahead
 * into the row queue would otherwise ask for hundreds of pages at a time,
 * most of which the scan never reads.
 */
#define LIONVEC_PREFETCH_MAX	64

extern double lionvec_epsilon;
extern int	lionvec_batch;
extern double lionvec_margin;
extern int	lionvec_patience;
extern int	lionvec_exact_rows;
extern double lionvec_exact_per_part;
extern int	lionvec_overflow;
extern int	lionvec_postfilter;
extern int	lionvec_build_sample;
extern int	lionvec_prefetch;	/* LION_READAHEAD_* (lion_readahead.h) */
extern bool lionvec_toast_direct;
extern bool lionvec_parallel_scan;

/*
 * What an ORDER BY operator of lionvec's returns: pgvector's all return
 * float8, and a hand-made class's may return float4, which the scan reads
 * and hands back as such (index_store_float8_orderby_distances() is core's
 * way); nothing else orders.
 */
static inline bool
lionvec_distance_type_ok(Oid type)
{
	return type == FLOAT8OID || type == FLOAT4OID;
}

/* lion_vec.c */
extern void lion_vec_init(void);
extern void lionvec_refuse_rls(Oid relid, const char *index);
extern double lionvec_exact_limit(double nparts);
extern LionVecInfo *lionvec_get_info(Relation index);
extern const float *lionvec_datum_floats(Datum d, bool half, float *buf,
										 int bufdim, int *dim, Pointer *tofree);
extern void lionvec_check_dims(const LionVecInfo *info, int dim);
extern void lionvec_read_meta(Relation index, LionVecMetaData *meta);
extern void lionvec_meta_copy(Relation index, Page page, LionVecMetaData *meta);
extern Buffer lionvec_new_buffer(Relation index, ForkNumber fork);
extern void lionvec_init_page(Page page, uint16 type);
extern bool lionvec_vector_inline(Relation index, Relation heap, int dims,
								  bool half);
extern bool lionvec_reads_placed(Relation heap, const LionVecMetaData *meta,
								 int dims, bool half);
extern void lionvec_check_page(Relation index, Page page, BlockNumber blk,
							   uint16 type);
extern Buffer lionvec_insert_readbuf(Relation index, BlockNumber blk);
extern LionVecCode *lionvec_code_at(Relation index, Page page, BlockNumber blk,
									OffsetNumber off, int dimsp, bool copies);
extern LionVecRefine *lionvec_refine_at(Relation index, Page page,
										BlockNumber blk, OffsetNumber off,
										int dimsp, int reflocruns,
										ItemPointer tid);
extern LionVecPart *lionvec_part_at(Relation index, Page page, BlockNumber blk,
									OffsetNumber off, int dimsp);
extern LionVecRegion *lionvec_region_at(Relation index, Page page,
										BlockNumber blk, OffsetNumber off,
										int dimsp);
extern BlockNumber *lionvec_chain_heads(Relation index,
										const LionVecMetaData *meta, int *n);
extern int	lionvec_page_items(Size itemsz);

/*
 * lion_vec_loc.c: the locator (DESIGN.md §44.13).  A heap's TOAST relation,
 * open for an index's lookups of its values' chunks, and the build's writer
 * of LOC and LOCDIR pages.  An entry's value is one of a TOAST pointer of a
 * 4-byte id: PostgreSQL 20's varatt_external_oid, before it struct
 * varatt_external.
 */
#if PG_VERSION_NUM >= 200000
typedef varatt_external_oid LionToastPointer;
#else
typedef struct varatt_external LionToastPointer;
#endif

typedef struct LionVecToast
{
	Relation	toastrel;		/* NULL: none */
	Oid			toastrelid;
	Relation   *idxs;			/* its indexes */
	int			nidx;
	int			valid;
	int			dims;
	bool		half;
	int64		extsize;		/* a value's bytes in TOAST */
	int			nchunks;		/* ... and its chunks */
} LionVecToast;

typedef struct LionVecLocWriter
{
	Relation	index;
	Buffer		buf;			/* the LOC page being filled */
	LionVecLocDir *dir;			/* an item a LOC page written */
	int			ndir;
	int			maxdir;
	ItemPointerData last;		/* the last entry's TID */
	double		nentries;
} LionVecLocWriter;

extern bool lionvec_toast_open(LionVecToast *t, Relation heap, int dims,
							   bool half);
extern void lionvec_toast_close(LionVecToast *t);
extern bool lionvec_toast_value(const LionVecToast *t, Datum d,
								LionToastPointer *ptr);
extern bool lionvec_toast_ptr_ok(const LionVecToast *t,
								 const LionToastPointer *ptr);
extern bool lionvec_toast_locate(LionVecToast *t,
								 const LionToastPointer *ptr,
								 char *data, LionVecLoc *loc);
extern bool lionvec_stored_pointer(Relation heap, ItemPointer tid,
								   AttrNumber attno,
								   LionToastPointer *ptr);
extern bool lionvec_place_run(Relation toastrel, Snapshot tsnap, Buffer buf,
							  const LionVecLocRun *run, Oid valueid, int seq0,
							  int64 extsize, char *data);
extern bool lionvec_place_read(Relation toastrel, Snapshot tsnap,
							   BlockNumber nblocks, Oid valueid,
							   const LionVecLocRun *runs, int nruns,
							   int64 extsize, char *data);
extern LionVecLoc *lionvec_loc_at(Relation index, Page page, BlockNumber blk,
								  OffsetNumber off, int nchunks);
extern LionVecLocDir *lionvec_locdir_read(Relation index,
										  const LionVecMetaData *meta, int *n);
extern void lionvec_loc_writer_begin(LionVecLocWriter *w, Relation index);
extern void lionvec_loc_writer_put(LionVecLocWriter *w, const LionVecLoc *loc);
extern void lionvec_loc_writer_end(LionVecLocWriter *w, LionVecMetaData *meta);
extern void lionvec_loc_append(Relation index, Buffer metabuf,
							   const LionVecMetaData *meta,
							   const LionVecLoc *loc);
extern void lionvec_vacuum_locs(Relation index, const LionVecMetaData *meta,
								int nchunks, IndexBulkDeleteCallback callback,
								void *callback_state,
								BufferAccessStrategy strategy);

/* lion_vec_scan.c: the scan callbacks */
extern IndexScanDesc lionvecbeginscan(Relation index, int nkeys, int norderbys);
extern void lionvecrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
						  ScanKey orderbys, int norderbys);
extern bool lionvecgettuple(IndexScanDesc scan, ScanDirection dir);
extern void lionvecendscan(IndexScanDesc scan);

/* ... and a parallel scan's (DESIGN.md §44.4, "A parallel scan") */
#if PG_VERSION_NUM >= 180000
extern Size lionvecestimateparallelscan(Relation index, int nkeys, int norderbys);
#elif PG_VERSION_NUM >= 170000
extern Size lionvecestimateparallelscan(int nkeys, int norderbys);
#else
extern Size lionvecestimateparallelscan(void);
#endif
extern void lionvecinitparallelscan(void *target);
extern void lionvecparallelrescan(IndexScanDesc scan);

/*
 * For LionOrdered (DESIGN.md §44.6): is this a lionvec index - asked of the
 * planner's IndexOptInfo or of the open relation?  By the access method's own
 * routines, not its name, so that an access method of that name some other
 * handler serves is never taken for one.
 */
extern bool lion_vec_index(IndexOptInfo *index);
extern bool lion_vec_relation(Relation index);

/*
 * ... and restrict a lionvec scan, after its
 * rescan and before its first tuple, to the members of a set of heap TIDs -
 * n container keys in ascending order and their containers.  The arrays
 * and the containers must outlive the scan, or the next rescan.
 */
extern void lion_vec_restrict(IndexScanDesc scan, int n, const uint32 *keys,
							  LionContainer *const *conts);

/*
 * ... and tell it, likewise after its rescan and before its first tuple,
 * how many rows its query will take from it, when the planner knew: the
 * LIMIT, OFFSET included, of the query level it is in (§44.4, "A known
 * LIMIT"; §44.6).  The scan then pauses at no tier below that, and
 * gathers that many in a pass when it is more than pg_lion.vector_batch,
 * to an eighth of work_mem's rows.  -1, as a rescan leaves it, is not
 * known.  A hint about where to pause and what to gather, never a bound
 * on the rows returned: a scan asked for more returns more.
 */
extern void lion_vec_scan_limit(IndexScanDesc scan, int64 rows);

/*
 * ... and what a scan of path's order restricted to `members` of the
 * index's rows costs before its first row, for the planner (§44.7), and
 * *fetch, the page each row it returns costs then: a random heap page,
 * or none where the exact path read it before the first.
 */
extern Cost lion_vec_scan_cost(PlannerInfo *root, IndexPath *path,
							   double members, Cost *fetch);

/*
 * ... and the price of a partial path of a lionvec order, which core gives
 * its workers only after amcostestimate has priced it as a serial scan: its
 * probes and its rows divided among the processes (§44.7), called by
 * LionOrdered's set_rel_pathlist_hook for each such path.
 */
extern void lion_vec_parallel_cost(PlannerInfo *root, IndexPath *path);

#endif							/* LION_VEC_H */
