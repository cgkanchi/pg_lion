/*-------------------------------------------------------------------------
 *
 * lion_vec.c
 *		lionvec, the nearest-neighbour index of pgvector's `vector`: its
 *		handler, settings, pages, build, inserts, VACUUM and costs.  The scan
 *		is lion_vec_scan.c, the arithmetic lion_vec_math.c (DESIGN.md §44).
 *
 * The index is three chains of pages under a meta page:
 *
 *	MEAN	the indexed rows' mean, rotated, as floats; written by the build
 *			and never changed
 *	CODE	one item a row: its TID, plane 1 and the numbers its estimate
 *			needs; what every scan reads whole.  The build writes these pages
 *			last and in one run, so that a scan reads them in block order.
 *	REFINE	one item a row with a vector: its 8-bit codes, which only the
 *			rows plane 1 cannot rule out are read for
 *	DIR		the heap block of the first row of each CODE page the build
 *			wrote, so that a scan restricted to a set of rows (LionOrdered)
 *			reads only the CODE pages that can hold one
 *
 * Inserts append to the tails, serialized by the meta page's lock.  VACUUM
 * removes CODE items, which nothing points at, and then their REFINE items,
 * which a scan finds through the CODE item it copied and checks by TID.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/amvalidate.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"

#include "lion_compat.h"
#include "lion_vec.h"

PG_FUNCTION_INFO_V1(lionvec_handler);

/* pg_lion.vector_epsilon and pg_lion.vector_batch (§44.8) */
double		lionvec_epsilon = 3.0;
int			lionvec_batch = 100;

static relopt_kind lionvec_relopt_kind;

static void lionveccostestimate(PlannerInfo *root, IndexPath *path,
								double loop_count, Cost *indexStartupCost,
								Cost *indexTotalCost,
								Selectivity *indexSelectivity,
								double *indexCorrelation, double *indexPages);

typedef struct LionVecOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
} LionVecOptions;

/*
 * pgvector's `vector`, as it lays it out: a varlena of a 16-bit dimension
 * count, 16 unused bits and the floats.  pg_lion reads it here rather than
 * linking pgvector, so that pg_lion itself loads without pgvector.
 */
typedef struct LionVecPgVector
{
	int32		vl_len_;
	int16		dim;
	int16		unused;
	float		x[FLEXIBLE_ARRAY_MEMBER];
} LionVecPgVector;

StaticAssertDecl(LIONVEC_CODE_HDRSZ == 32,
				 "lionvec: the CODE item header is four words");
StaticAssertDecl(offsetof(LionVecRefine, codes) == 16,
				 "lionvec: the REFINE item header is two words");
StaticAssertDecl(LIONVEC_MAX_DIM >= 2000,
				 "lionvec: the page size takes the dimensions pgvector's own indexes do");

/* ----------------------------------------------------------------
 * Settings
 * ----------------------------------------------------------------
 */

void
lion_vec_init(void)
{
	lionvec_tables_init();
	lionvec_kernels_init();

	lionvec_relopt_kind = add_reloption_kind();

	DefineCustomRealVariable("pg_lion.vector_epsilon",
							 "Width of a lionvec scan's error bounds, in standard deviations.",
							 "A row whose bound leaves it out when it belongs is missed; "
							 "each bound fails with the Gaussian's probability of this many standard deviations, "
							 "and wider bounds make the scan read and compare more rows.",
							 &lionvec_epsilon,
							 3.0, 1.0, 10.0,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_lion.vector_batch",
							"Rows a lionvec scan gathers candidates for in its first pass.",
							"Every pass reads plane 1 of every row; a query that wants more rows than a pass gathered "
							"makes another, gathering four times as many.",
							&lionvec_batch,
							100, 1, 100000,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
}

bool
lion_vec_relation(Relation index)
{
	return index->rd_indam != NULL &&
		index->rd_indam->ambeginscan == lionvecbeginscan;
}

/* ----------------------------------------------------------------
 * Vectors
 * ----------------------------------------------------------------
 */

/*
 * The floats of a `vector` datum and their count; *tofree is the detoasted
 * copy to pfree, or NULL.
 */
const float *
lionvec_datum_floats(Datum d, int *dim, Pointer *tofree)
{
	struct varlena *raw = (struct varlena *) DatumGetPointer(d);
	struct varlena *v = pg_detoast_datum(raw);
	LionVecPgVector *vec = (LionVecPgVector *) v;
	Size		len = VARSIZE(v);

	*tofree = v != raw ? (Pointer) v : NULL;
	if (len < offsetof(LionVecPgVector, x) || vec->dim < 0 ||
		len < offsetof(LionVecPgVector, x) + sizeof(float) * (Size) vec->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("lionvec was given a malformed vector of %zu bytes", len)));
	*dim = vec->dim;
	return vec->x;
}

/* pgvector's own error, so a query reads the same with or without the index */
void
lionvec_check_dims(const LionVecInfo *info, int dim)
{
	if (dim != info->dims)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", info->dims, dim)));
}

static void
lionvec_check_max_dims(int dims)
{
	if (dims < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));
	if (dims > LIONVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("column cannot have more than %d dimensions for lionvec index",
						LIONVEC_MAX_DIM)));
}

/* The metric of the index's operator class: its one ORDER BY strategy. */
static int
lionvec_index_metric(Relation index)
{
	Oid			family = index->rd_opfamily[0];
	Oid			type = index->rd_opcintype[0];
	int			s;

	for (s = 1; s <= LIONVEC_NSTRATEGIES; s++)
		if (OidIsValid(get_opfamily_member(family, type, type, s)))
			return s;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			 errmsg("operator family %u of lionvec index \"%s\" has no distance operator",
					family, RelationGetRelationName(index))));
	return 0;					/* keep the compiler quiet */
}

/* ----------------------------------------------------------------
 * Pages
 * ----------------------------------------------------------------
 */

static void
lionvec_init_page(Page page, uint16 type)
{
	LionVecPageOpaque op;

	PageInit(page, BLCKSZ, sizeof(LionVecPageOpaqueData));
	op = LionVecPageGetOpaque(page);
	op->next = InvalidBlockNumber;
	op->minkey = PG_UINT32_MAX;
	op->maxkey = 0;
	op->type = type;
	op->page_id = LIONVEC_PAGE_ID;
}

/* What a page of a chain must look like before anything on it is read. */
void
lionvec_check_page(Relation index, Page page, BlockNumber blk, uint16 type)
{
	LionVecPageOpaque op;

	if (PageIsNew(page) ||
		PageGetSpecialSize(page) != MAXALIGN(sizeof(LionVecPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has an uninitialized or foreign page at block %u",
						RelationGetRelationName(index), blk)));
	op = LionVecPageGetOpaque(page);
	if (op->page_id != LIONVEC_PAGE_ID || op->type != type)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a page of type %u where one of type %u belongs, at block %u",
						RelationGetRelationName(index), op->type, type, blk)));
}

/*
 * A CODE item, checked against the length a row of its kind has: NULL rows
 * and cosine zero vectors carry the header alone, every other row its plane.
 */
LionVecCode *
lionvec_code_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
				int dimsp)
{
	ItemId		iid = PageGetItemId(page, off);
	LionVecCode *code;
	Size		len;

	if (!ItemIdIsNormal(iid) || ItemIdGetLength(iid) < LIONVEC_CODE_HDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed item at (%u,%u)",
						RelationGetRelationName(index), blk, off)));
	code = (LionVecCode *) PageGetItem(page, iid);
	len = ItemIdGetLength(iid);
	if ((code->flags & ~(LIONVEC_NULL | LIONVEC_ZERO)) != 0 ||
		((code->flags & (LIONVEC_NULL | LIONVEC_ZERO)) != 0 ?
		 len != LIONVEC_CODE_HDRSZ :
		 (dimsp <= 0 || len != LIONVEC_CODE_SIZE(dimsp))))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has an item of %zu bytes and flags 0x%x at (%u,%u)",
						RelationGetRelationName(index), len, code->flags, blk, off)));
	return code;
}

/*
 * The REFINE item of the row tid at (blk, off), or NULL when it is not there
 * any more: VACUUM removed it, and the slot may hold another row's since
 * (§44.5).  A row VACUUM removes is dead to every snapshot, so a scan that
 * finds it gone loses nothing.
 */
LionVecRefine *
lionvec_refine_at(Relation index, Page page, BlockNumber blk, OffsetNumber off,
				  int dimsp, ItemPointer tid)
{
	ItemId		iid;
	LionVecRefine *ref;

	if (off < FirstOffsetNumber || off > PageGetMaxOffsetNumber(page))
		return NULL;
	iid = PageGetItemId(page, off);
	if (!ItemIdIsNormal(iid))
		return NULL;
	if (ItemIdGetLength(iid) != LIONVEC_REFINE_SIZE(dimsp))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a refine item of %u bytes at (%u,%u)",
						RelationGetRelationName(index), ItemIdGetLength(iid), blk, off)));
	ref = (LionVecRefine *) PageGetItem(page, iid);
	return ItemPointerEquals(&ref->tid, tid) ? ref : NULL;
}

static void
lionvec_check_meta(Relation index, Page page)
{
	LionVecMeta meta = LionVecPageGetMeta(page);

	lionvec_check_page(index, page, LIONVEC_METAPAGE, LIONVEC_PAGE_META);
	if (meta->magic != LIONVEC_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a lionvec index",
						RelationGetRelationName(index))));
	if (meta->version != LIONVEC_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has format version %u, and this pg_lion reads version %d",
						RelationGetRelationName(index), meta->version, LIONVEC_VERSION),
				 errhint("REINDEX it.")));
	if (meta->metric < 1 || meta->metric > LIONVEC_NSTRATEGIES ||
		meta->dims < 0 || meta->dims > LIONVEC_MAX_DIM ||
		meta->dimsp != LIONVEC_PADDED(meta->dims) ||
		!BlockNumberIsValid(meta->code_head) || !BlockNumberIsValid(meta->code_tail) ||
		(meta->nbuilt > 0) != BlockNumberIsValid(meta->dir_head) ||
		meta->nbuilt > MaxBlockNumber - meta->code_head)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a malformed meta page",
						RelationGetRelationName(index))));
}

/* A copy of the meta page, read under a share lock. */
void
lionvec_read_meta(Relation index, LionVecMetaData *meta)
{
	Buffer		buf = ReadBuffer(index, LIONVEC_METAPAGE);
	Page		page;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lionvec_check_meta(index, page);
	*meta = *LionVecPageGetMeta(page);
	UnlockReleaseBuffer(buf);
}

static void
lionvec_init_meta(Page page, int metric, int dims)
{
	LionVecMeta meta;

	lionvec_init_page(page, LIONVEC_PAGE_META);
	meta = LionVecPageGetMeta(page);
	memset(meta, 0, sizeof(LionVecMetaData));
	meta->magic = LIONVEC_MAGIC;
	meta->version = LIONVEC_VERSION;
	meta->metric = (uint16) metric;
	meta->dims = dims;
	meta->dimsp = LIONVEC_PADDED(dims);
	meta->seed = LIONVEC_SEED;
	meta->mean_head = InvalidBlockNumber;
	meta->code_head = InvalidBlockNumber;
	meta->code_tail = InvalidBlockNumber;
	meta->refine_tail = InvalidBlockNumber;
	meta->dir_head = InvalidBlockNumber;
	meta->nbuilt = 0;
	((PageHeader) page)->pd_lower =
		((char *) meta + sizeof(LionVecMetaData)) - (char *) page;
}

/* floats a MEAN page holds */
#define LIONVEC_MEAN_PER_PAGE \
	((int) ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
			 MAXALIGN(sizeof(LionVecPageOpaqueData))) / sizeof(float)))

/* ----------------------------------------------------------------
 * The per-index information a backend keeps (rd_amcache)
 * ----------------------------------------------------------------
 */

static Size
lionvec_info_size(int dimsp)
{
	Size		size = MAXALIGN(sizeof(LionVecInfo));

	if (dimsp > 0)
		size += sizeof(float) * (lionvec_rotation_nsigns(dimsp) + dimsp);
	return size;
}

/* Point a copy's arrays at its own trailing storage. */
static void
lionvec_info_fix(LionVecInfo *info, bool hasmean)
{
	float	   *tail = (float *) ((char *) info + MAXALIGN(sizeof(LionVecInfo)));

	info->rot.signs = NULL;
	info->mean = NULL;
	if (info->dims > 0)
	{
		info->rot.signs = tail;
		if (hasmean)
			info->mean = tail + lionvec_rotation_nsigns(info->dimsp);
	}
}

static void
lionvec_read_mean(Relation index, BlockNumber blk, float *mean, int dimsp)
{
	int			done = 0;
	uint64		visited = 0;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);

	while (done < dimsp)
	{
		Buffer		buf;
		Page		page;
		int			n = Min(dimsp - done, LIONVEC_MEAN_PER_PAGE);

		if (!BlockNumberIsValid(blk))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a short mean",
							RelationGetRelationName(index))));
		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_MEAN);
		if (((PageHeader) page)->pd_lower !=
			MAXALIGN(SizeOfPageHeaderData) + sizeof(float) * n)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a malformed mean page at block %u",
							RelationGetRelationName(index), blk)));
		memcpy(mean + done, PageGetContents(page), sizeof(float) * n);
		done += n;
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
	}
}

/*
 * The DIR chain (lion_vec.h): meta->nbuilt heap blocks, palloc'd, or NULL
 * when the index has none.  A directory that is not in order, or does not
 * fit the relation, is corrupt: a scan would skip pages it must read.
 */
BlockNumber *
lionvec_read_dir(Relation index, const LionVecMetaData *meta)
{
	BlockNumber *dir;
	BlockNumber blk = meta->dir_head;
	uint32		done = 0;
	uint64		visited = 0;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);

	if (meta->nbuilt == 0)
		return NULL;
	if (meta->nbuilt > nblocks || meta->code_head > nblocks - meta->nbuilt)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("lionvec index \"%s\" has a directory of %u pages from block %u, and %u blocks",
						RelationGetRelationName(index), meta->nbuilt, meta->code_head, nblocks)));
	dir = palloc(sizeof(BlockNumber) * meta->nbuilt);
	while (done < meta->nbuilt)
	{
		Buffer		buf;
		Page		page;
		uint32		n = Min(meta->nbuilt - done, (uint32) LIONVEC_DIR_PER_PAGE);
		uint32		i;

		if (!BlockNumberIsValid(blk))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a short directory",
							RelationGetRelationName(index))));
		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_DIR);
		if (((PageHeader) page)->pd_lower !=
			MAXALIGN(SizeOfPageHeaderData) + sizeof(BlockNumber) * n)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("lionvec index \"%s\" has a malformed directory page at block %u",
							RelationGetRelationName(index), blk)));
		memcpy(dir + done, PageGetContents(page), sizeof(BlockNumber) * n);
		blk = LionVecPageGetOpaque(page)->next;
		UnlockReleaseBuffer(buf);
		for (i = done; i < done + n; i++)
		{
			if (i > 0 && dir[i] < dir[i - 1])
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("lionvec index \"%s\" has a directory out of order at entry %u",
								RelationGetRelationName(index), i)));
		}
		done += n;
	}
	return dir;
}

/*
 * The rotation and mean of the index, as a palloc'd copy the caller owns.
 * The backend keeps one in rd_amcache, made again when the meta page says
 * something else; a copy is returned rather than that one because a relcache
 * invalidation, which can arrive whenever a lock is taken (a TOAST fetch's
 * included), frees it.
 */
LionVecInfo *
lionvec_get_info(Relation index)
{
	LionVecMetaData meta;
	LionVecInfo *cached = (LionVecInfo *) index->rd_amcache;
	LionVecInfo *copy;
	Size		size;

	lionvec_read_meta(index, &meta);
	if (cached == NULL || cached->dims != meta.dims ||
		cached->metric != meta.metric || cached->seed != meta.seed)
	{
		if (cached != NULL)
		{
			index->rd_amcache = NULL;
			pfree(cached);
		}
		size = lionvec_info_size(meta.dimsp);
		cached = MemoryContextAllocZero(index->rd_indexcxt, size);
		cached->metric = meta.metric;
		cached->dims = meta.dims;
		cached->dimsp = meta.dimsp;
		cached->seed = meta.seed;
		lionvec_info_fix(cached, BlockNumberIsValid(meta.mean_head));
		if (cached->dims > 0)
		{
			lionvec_rotation_init(&cached->rot, cached->dimsp, cached->seed,
								  cached->rot.signs);
			if (cached->mean != NULL)
				lionvec_read_mean(index, meta.mean_head, cached->mean,
								  cached->dimsp);
		}
		index->rd_amcache = cached;
	}

	size = lionvec_info_size(cached->dimsp);
	copy = palloc(size);
	memcpy(copy, cached, size);
	lionvec_info_fix(copy, cached->mean != NULL);
	return copy;
}

/*
 * The first vector an index built without a dimension count is given fixes
 * it (§44.2): under the meta page's lock, so that two first inserts agree.
 */
static void
lionvec_set_dims(Relation index, int dims)
{
	Buffer		buf;
	Page		page;
	LionVecMeta meta;

	lionvec_check_max_dims(dims);
	buf = ReadBuffer(index, LIONVEC_METAPAGE);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	lionvec_check_meta(index, page);
	if (LionVecPageGetMeta(page)->dims == 0 && dims > 0)
	{
		GenericXLogState *state = GenericXLogStart(index);

		page = GenericXLogRegisterBuffer(state, buf, 0);
		meta = LionVecPageGetMeta(page);
		meta->dims = dims;
		meta->dimsp = LIONVEC_PADDED(dims);
		GenericXLogFinish(state);
	}
	UnlockReleaseBuffer(buf);
}

/* ----------------------------------------------------------------
 * Encoding a row into its items
 * ----------------------------------------------------------------
 */

typedef struct LionVecItems
{
	LionVecCode *code;			/* LIONVEC_CODE_SIZE(dimsp) bytes */
	Size		codesize;
	LionVecRefine *ref;			/* LIONVEC_REFINE_SIZE(dimsp) bytes */
	bool		hasref;
	float	   *x;				/* [dimsp] */
} LionVecItems;

static void
lionvec_items_alloc(LionVecItems *it, int dimsp)
{
	it->code = palloc0(LIONVEC_CODE_SIZE(Max(dimsp, 64)));
	it->ref = palloc0(LIONVEC_REFINE_SIZE(Max(dimsp, 64)));
	it->x = palloc(sizeof(float) * Max(dimsp, 64));
}

/*
 * Fill it with the items of the row tid: value its datum, or isnull.  The
 * caller has checked value's dimensions against info.
 */
static void
lionvec_items_fill(const LionVecInfo *info, LionVecItems *it, ItemPointer tid,
				   Datum value, bool isnull)
{
	LionVecCode *code = it->code;
	LionVecEncoded e;

	memset(code, 0, LIONVEC_CODE_HDRSZ);
	code->tid = *tid;
	code->rblk = InvalidBlockNumber;
	code->roff = InvalidOffsetNumber;
	it->codesize = LIONVEC_CODE_HDRSZ;
	it->hasref = false;

	if (isnull)
	{
		code->flags = LIONVEC_NULL;
		return;
	}
	else
	{
		Pointer		tofree;
		int			dim;
		const float *v = lionvec_datum_floats(value, &dim, &tofree);
		bool		nonzero;

		lionvec_check_dims(info, dim);
		if (info->dims == 0)
			elog(ERROR, "lionvec: no dimensions to encode a vector in");
		nonzero = lionvec_prepare(&info->rot, info->metric, v, dim, it->x);
		if (tofree != NULL)
			pfree(tofree);
		if (!nonzero)
		{
			code->flags = LIONVEC_ZERO;
			return;
		}
	}

	e.bits = code->bits;
	e.codes = it->ref->codes;
	lionvec_encode(info->mean, info->dimsp, it->x, &e);
	code->norm = e.norm;
	code->alpha1 = e.alpha1;
	code->xm = e.xm;
	code->popcount = e.popcount;
	it->codesize = LIONVEC_CODE_SIZE(info->dimsp);

	it->ref->tid = *tid;
	it->ref->pad = 0;
	it->ref->alpha8 = e.alpha8;
	it->ref->rnorm8 = e.rnorm8;
	it->hasref = true;
}

static void
lionvec_note_key(Page page, ItemPointer tid)
{
	LionVecPageOpaque op = LionVecPageGetOpaque(page);
	uint32		ckey = lionvec_ckey(tid);

	op->minkey = Min(op->minkey, ckey);
	op->maxkey = Max(op->maxkey, ckey);
}

/* ----------------------------------------------------------------
 * Build
 * ----------------------------------------------------------------
 */

typedef struct LionVecBuildState
{
	Relation	index;
	int			metric;
	int			dims;
	/* the first pass: the mean */
	double	   *sum;			/* [dims] */
	double		nsum;
	/* the second: the items */
	LionVecInfo *info;
	LionVecItems items;
	Buffer		rbuf;			/* the REFINE page being filled */
	BlockNumber refine_tail;
	BufFile    *spool;			/* the CODE items, written out last */
	BlockNumber lastblk;		/* the heap block of the last row spooled */
	bool		ordered;		/* every row's block >= the one before's */
	double		indtuples;
	MemoryContext buildcxt;		/* what lasts the build */
	MemoryContext tmpcxt;		/* reset after each row */
} LionVecBuildState;

static Buffer
lionvec_new_buffer(Relation index, ForkNumber fork)
{
	return ExtendBufferedRel(BMR_REL(index), fork, NULL, EB_LOCK_FIRST);
}

static void
lionvec_build_mean_callback(Relation index, ItemPointer tid, Datum *values,
							bool *isnull, bool tupleIsAlive, void *state)
{
	LionVecBuildState *bs = (LionVecBuildState *) state;
	MemoryContext old;
	Pointer		tofree;
	const float *v;
	int			dim;
	int			i;
	double		scale = 1.0;

	if (isnull[0])
		return;
	old = MemoryContextSwitchTo(bs->tmpcxt);
	v = lionvec_datum_floats(values[0], &dim, &tofree);
	if (bs->dims == 0)
	{
		lionvec_check_max_dims(dim);
		bs->dims = dim;
	}
	if (dim != bs->dims)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", bs->dims, dim)));
	if (bs->sum == NULL && dim > 0)
		bs->sum = MemoryContextAllocZero(bs->buildcxt, sizeof(double) * dim);
	if (bs->metric == LIONVEC_COSINE)
	{
		double		n2 = 0.0;

		for (i = 0; i < dim; i++)
			n2 += (double) v[i] * (double) v[i];
		scale = n2 > 0.0 ? 1.0 / sqrt(n2) : 0.0;
	}
	if (scale > 0.0 && dim > 0)
	{
		for (i = 0; i < dim; i++)
			bs->sum[i] += v[i] * scale;
		bs->nsum += 1.0;
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(bs->tmpcxt);
}

/* Put a REFINE item on the page being filled, or a new one. */
static void
lionvec_build_put_refine(LionVecBuildState *bs, LionVecRefine *ref, Size size,
						 BlockNumber *blk, OffsetNumber *off)
{
	Page		page;

	if (BufferIsValid(bs->rbuf) &&
		PageGetFreeSpace(BufferGetPage(bs->rbuf)) < MAXALIGN(size))
	{
		MarkBufferDirty(bs->rbuf);
		UnlockReleaseBuffer(bs->rbuf);
		bs->rbuf = InvalidBuffer;
	}
	if (!BufferIsValid(bs->rbuf))
	{
		bs->rbuf = lionvec_new_buffer(bs->index, MAIN_FORKNUM);
		lionvec_init_page(BufferGetPage(bs->rbuf), LIONVEC_PAGE_REFINE);
		bs->refine_tail = BufferGetBlockNumber(bs->rbuf);
	}
	page = BufferGetPage(bs->rbuf);
	*off = PageAddItem(page, (Item) ref, size, InvalidOffsetNumber, false, false);
	if (*off == InvalidOffsetNumber)
		elog(ERROR, "lionvec: could not add a refine item of %zu bytes", size);
	*blk = bs->refine_tail;
}

static void
lionvec_build_callback(Relation index, ItemPointer tid, Datum *values,
					   bool *isnull, bool tupleIsAlive, void *state)
{
	LionVecBuildState *bs = (LionVecBuildState *) state;
	LionVecItems *it = &bs->items;
	MemoryContext old = MemoryContextSwitchTo(bs->tmpcxt);
	uint16		len;

	lionvec_items_fill(bs->info, it, tid, values[0], isnull[0]);
	if (it->hasref)
		lionvec_build_put_refine(bs, it->ref, LIONVEC_REFINE_SIZE(bs->info->dimsp),
								 &it->code->rblk, &it->code->roff);
	/* the directory needs the rows in heap order (lionvec_build_write_codes()) */
	if (bs->indtuples > 0 && ItemPointerGetBlockNumber(tid) < bs->lastblk)
		bs->ordered = false;
	bs->lastblk = ItemPointerGetBlockNumber(tid);
	len = (uint16) it->codesize;
	BufFileWrite(bs->spool, &len, sizeof(len));
	BufFileWrite(bs->spool, it->code, len);
	bs->indtuples += 1;

	MemoryContextSwitchTo(old);
	MemoryContextReset(bs->tmpcxt);
}

/* Write the mean, rotated, as a chain of MEAN pages; returns its head. */
static BlockNumber
lionvec_build_write_mean(Relation index, const float *mean, int dimsp)
{
	BlockNumber head = InvalidBlockNumber;
	Buffer		prev = InvalidBuffer;
	int			done = 0;

	while (done < dimsp)
	{
		Buffer		buf = lionvec_new_buffer(index, MAIN_FORKNUM);
		Page		page = BufferGetPage(buf);
		int			n = Min(dimsp - done, LIONVEC_MEAN_PER_PAGE);

		lionvec_init_page(page, LIONVEC_PAGE_MEAN);
		memcpy(PageGetContents(page), mean + done, sizeof(float) * n);
		((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + sizeof(float) * n;
		done += n;
		if (BufferIsValid(prev))
		{
			LionVecPageGetOpaque(BufferGetPage(prev))->next = BufferGetBlockNumber(buf);
			MarkBufferDirty(prev);
			UnlockReleaseBuffer(prev);
		}
		else
			head = BufferGetBlockNumber(buf);
		prev = buf;
	}
	if (BufferIsValid(prev))
	{
		MarkBufferDirty(prev);
		UnlockReleaseBuffer(prev);
	}
	return head;
}

/*
 * The CODE pages, from the spool, in one run of blocks: a scan reads them in
 * order, and the operating system's read-ahead with it.  Returns the heap
 * block of each page's first row, *npages of them, for the directory - or
 * NULL when the rows did not come in heap order, or the pages are not one
 * run, and the directory would not hold (lion_vec.h).
 */
static BlockNumber *
lionvec_build_write_codes(LionVecBuildState *bs, LionVecMeta meta,
						  uint32 *npages)
{
	Relation	index = bs->index;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	char	   *item = palloc(LIONVEC_CODE_SIZE(LIONVEC_MAX_DIM));
	uint16		len;
	BlockNumber *first = NULL;
	uint32		nfirst = 0;
	uint32		maxfirst = 0;
	bool		run = bs->ordered;

	if (BufFileSeek(bs->spool, 0, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("lionvec: could not rewind the build's temporary file")));
	for (;;)
	{
		size_t		got = BufFileReadMaybeEOF(bs->spool, &len, sizeof(len), true);

		if (got == 0)
			break;
		if (len < LIONVEC_CODE_HDRSZ || len > LIONVEC_CODE_SIZE(LIONVEC_MAX_DIM))
			elog(ERROR, "lionvec: the build's temporary file has an item of %u bytes", len);
		BufFileReadExact(bs->spool, item, len);
		CHECK_FOR_INTERRUPTS();

		if (BufferIsValid(buf) && PageGetFreeSpace(page) < MAXALIGN(len))
		{
			Buffer		next = lionvec_new_buffer(index, MAIN_FORKNUM);

			if (BufferGetBlockNumber(next) != BufferGetBlockNumber(buf) + 1)
				run = false;
			LionVecPageGetOpaque(page)->next = BufferGetBlockNumber(next);
			MarkBufferDirty(buf);
			UnlockReleaseBuffer(buf);
			buf = next;
			page = BufferGetPage(buf);
			lionvec_init_page(page, LIONVEC_PAGE_CODE);
		}
		else if (!BufferIsValid(buf))
		{
			buf = lionvec_new_buffer(index, MAIN_FORKNUM);
			page = BufferGetPage(buf);
			lionvec_init_page(page, LIONVEC_PAGE_CODE);
			meta->code_head = BufferGetBlockNumber(buf);
		}
		if (PageGetMaxOffsetNumber(page) == 0 && run)
		{
			/* the page's first row */
			if (nfirst == maxfirst)
			{
				maxfirst = Max(maxfirst * 2, 1024);
				first = first == NULL ? palloc(sizeof(BlockNumber) * maxfirst) :
					repalloc(first, sizeof(BlockNumber) * maxfirst);
			}
			first[nfirst++] = ItemPointerGetBlockNumber(&((LionVecCode *) item)->tid);
		}
		if (PageAddItem(page, (Item) item, len, InvalidOffsetNumber, false,
						false) == InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add an item of %u bytes", len);
		lionvec_note_key(page, &((LionVecCode *) item)->tid);
	}

	/* an empty index has one empty CODE page, the tail inserts append to */
	if (!BufferIsValid(buf))
	{
		buf = lionvec_new_buffer(index, MAIN_FORKNUM);
		page = BufferGetPage(buf);
		lionvec_init_page(page, LIONVEC_PAGE_CODE);
		meta->code_head = BufferGetBlockNumber(buf);
	}
	meta->code_tail = BufferGetBlockNumber(buf);
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
	pfree(item);

	if (!run || nfirst == 0)
	{
		if (first != NULL)
			pfree(first);
		*npages = 0;
		return NULL;
	}
	*npages = nfirst;
	return first;
}

/* Write the directory as a chain of DIR pages; returns its head. */
static BlockNumber
lionvec_build_write_dir(Relation index, const BlockNumber *first, uint32 n)
{
	BlockNumber head = InvalidBlockNumber;
	Buffer		prev = InvalidBuffer;
	uint32		done = 0;

	while (done < n)
	{
		Buffer		buf = lionvec_new_buffer(index, MAIN_FORKNUM);
		Page		page = BufferGetPage(buf);
		uint32		k = Min(n - done, (uint32) LIONVEC_DIR_PER_PAGE);

		lionvec_init_page(page, LIONVEC_PAGE_DIR);
		memcpy(PageGetContents(page), first + done, sizeof(BlockNumber) * k);
		((PageHeader) page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + sizeof(BlockNumber) * k;
		done += k;
		if (BufferIsValid(prev))
		{
			LionVecPageGetOpaque(BufferGetPage(prev))->next = BufferGetBlockNumber(buf);
			MarkBufferDirty(prev);
			UnlockReleaseBuffer(prev);
		}
		else
			head = BufferGetBlockNumber(buf);
		prev = buf;
	}
	if (BufferIsValid(prev))
	{
		MarkBufferDirty(prev);
		UnlockReleaseBuffer(prev);
	}
	return head;
}

/* The dimensions the column's type modifier fixes, or 0. */
static int
lionvec_typmod_dims(Relation index)
{
	int32		typmod = TupleDescAttr(RelationGetDescr(index), 0)->atttypmod;

	if (typmod > 0)
	{
		lionvec_check_max_dims(typmod);
		return typmod;
	}
	return 0;
}

static IndexBuildResult *
lionvecbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	LionVecBuildState bs;
	Buffer		metabuf;
	LionVecMetaData meta;
	double		reltuples;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	memset(&bs, 0, sizeof(bs));
	bs.index = index;
	bs.metric = lionvec_index_metric(index);
	bs.dims = lionvec_typmod_dims(index);
	bs.rbuf = InvalidBuffer;
	bs.refine_tail = InvalidBlockNumber;
	bs.buildcxt = CurrentMemoryContext;
	bs.tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "lionvec build row",
									  ALLOCSET_DEFAULT_SIZES);

	/* the meta page is block 0, filled in last */
	metabuf = lionvec_new_buffer(index, MAIN_FORKNUM);
	Assert(BufferGetBlockNumber(metabuf) == LIONVEC_METAPAGE);
	lionvec_init_meta(BufferGetPage(metabuf), bs.metric, 0);
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	/*
	 * pass 1: the mean.  Neither pass joins a synchronized scan, which could
	 * start part-way through the heap: the second must see the rows in heap
	 * order for the directory.
	 */
	table_index_build_scan(heap, index, indexInfo, false, false,
						   lionvec_build_mean_callback, &bs, NULL);

	/* the rotation, and the mean rotated, in this backend's memory */
	{
		int			dimsp = LIONVEC_PADDED(bs.dims);
		Size		size = lionvec_info_size(dimsp);

		bs.info = palloc0(size);
		bs.info->metric = bs.metric;
		bs.info->dims = bs.dims;
		bs.info->dimsp = dimsp;
		bs.info->seed = LIONVEC_SEED;
		lionvec_info_fix(bs.info, bs.nsum > 0);
		if (bs.dims > 0)
			lionvec_rotation_init(&bs.info->rot, dimsp, LIONVEC_SEED,
								  bs.info->rot.signs);
		if (bs.info->mean != NULL)
		{
			int			i;

			for (i = 0; i < dimsp; i++)
				bs.info->mean[i] = i < bs.dims ? (float) (bs.sum[i] / bs.nsum) : 0.0f;
			lionvec_rotate(&bs.info->rot, bs.info->mean);
		}
	}

	memset(&meta, 0, sizeof(meta));
	meta.mean_head = InvalidBlockNumber;
	if (bs.info->mean != NULL)
		meta.mean_head = lionvec_build_write_mean(index, bs.info->mean,
												  bs.info->dimsp);

	/* pass 2: the REFINE items in place, the CODE items to a spool */
	lionvec_items_alloc(&bs.items, bs.info->dimsp);
	bs.spool = BufFileCreateTemp(false);
	bs.ordered = true;
	reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
									   lionvec_build_callback, &bs, NULL);
	if (BufferIsValid(bs.rbuf))
	{
		MarkBufferDirty(bs.rbuf);
		UnlockReleaseBuffer(bs.rbuf);
	}
	{
		uint32		nbuilt;
		BlockNumber *first = lionvec_build_write_codes(&bs, &meta, &nbuilt);

		BufFileClose(bs.spool);
		meta.dir_head = InvalidBlockNumber;
		meta.nbuilt = 0;
		if (first != NULL)
		{
			meta.dir_head = lionvec_build_write_dir(index, first, nbuilt);
			meta.nbuilt = nbuilt;
			pfree(first);
		}
	}

	/* the meta page */
	metabuf = ReadBuffer(index, LIONVEC_METAPAGE);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	{
		LionVecMeta m = LionVecPageGetMeta(BufferGetPage(metabuf));

		m->dims = bs.info->dims;
		m->dimsp = bs.info->dimsp;
		m->mean_head = meta.mean_head;
		m->code_head = meta.code_head;
		m->code_tail = meta.code_tail;
		m->refine_tail = bs.refine_tail;
		m->dir_head = meta.dir_head;
		m->nbuilt = meta.nbuilt;
		m->ntuples = bs.indtuples;
	}
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	if (RelationNeedsWAL(index))
		log_newpage_range(index, MAIN_FORKNUM, 0,
						  RelationGetNumberOfBlocks(index), true);

	MemoryContextDelete(bs.tmpcxt);

	result = palloc(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = bs.indtuples;
	return result;
}

/* An unlogged index's init fork: the meta page and an empty CODE page. */
static void
lionvecbuildempty(Relation index)
{
	int			metric = lionvec_index_metric(index);
	int			dims = lionvec_typmod_dims(index);
	Buffer		metabuf;
	Buffer		buf;
	LionVecMeta meta;

	metabuf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
								EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(metabuf) == LIONVEC_METAPAGE);
	buf = ExtendBufferedRel(BMR_REL(index), INIT_FORKNUM, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);

	START_CRIT_SECTION();
	lionvec_init_page(BufferGetPage(buf), LIONVEC_PAGE_CODE);
	MarkBufferDirty(buf);
	log_newpage_buffer(buf, true);

	lionvec_init_meta(BufferGetPage(metabuf), metric, dims);
	meta = LionVecPageGetMeta(BufferGetPage(metabuf));
	meta->code_head = BufferGetBlockNumber(buf);
	meta->code_tail = BufferGetBlockNumber(buf);
	MarkBufferDirty(metabuf);
	log_newpage_buffer(metabuf, true);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(metabuf);
}

/* ----------------------------------------------------------------
 * Insert
 * ----------------------------------------------------------------
 */

/*
 * Append a row's items: the REFINE item first, as the CODE item names it,
 * both in one WAL record with the meta page and any new page - at most four
 * buffers, which is what a GenericXLog record takes.  The meta page's lock
 * is held throughout, which serializes inserts; VACUUM and scans lock one
 * page at a time and never the meta page with another, so the order meta,
 * REFINE, CODE cannot deadlock against them.
 */
static void
lionvec_append(Relation index, LionVecItems *it, int dimsp)
{
	Buffer		metabuf;
	Buffer		rbuf = InvalidBuffer;
	Buffer		cbuf;
	Buffer		nbuf = InvalidBuffer;
	bool		rnew = false;
	Page		page;
	LionVecMetaData meta;
	GenericXLogState *state;
	Size		rsize = LIONVEC_REFINE_SIZE(dimsp);

	metabuf = ReadBuffer(index, LIONVEC_METAPAGE);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	lionvec_check_meta(index, BufferGetPage(metabuf));
	meta = *LionVecPageGetMeta(BufferGetPage(metabuf));
	if (it->hasref && meta.dimsp != dimsp)
		elog(ERROR, "lionvec: index \"%s\" changed dimensions under an insert",
			 RelationGetRelationName(index));

	if (it->hasref)
	{
		if (BlockNumberIsValid(meta.refine_tail))
		{
			rbuf = ReadBuffer(index, meta.refine_tail);
			LockBuffer(rbuf, BUFFER_LOCK_EXCLUSIVE);
			lionvec_check_page(index, BufferGetPage(rbuf), meta.refine_tail,
							   LIONVEC_PAGE_REFINE);
			if (PageGetFreeSpace(BufferGetPage(rbuf)) < MAXALIGN(rsize))
			{
				UnlockReleaseBuffer(rbuf);
				rbuf = InvalidBuffer;
			}
		}
		if (!BufferIsValid(rbuf))
		{
			rbuf = lionvec_new_buffer(index, MAIN_FORKNUM);
			rnew = true;
		}
	}

	cbuf = ReadBuffer(index, meta.code_tail);
	LockBuffer(cbuf, BUFFER_LOCK_EXCLUSIVE);
	lionvec_check_page(index, BufferGetPage(cbuf), meta.code_tail,
					   LIONVEC_PAGE_CODE);
	if (PageGetFreeSpace(BufferGetPage(cbuf)) < MAXALIGN(it->codesize))
		nbuf = lionvec_new_buffer(index, MAIN_FORKNUM);

	state = GenericXLogStart(index);
	if (it->hasref)
	{
		OffsetNumber off;

		page = GenericXLogRegisterBuffer(state, rbuf,
										 rnew ? GENERIC_XLOG_FULL_IMAGE : 0);
		if (rnew)
			lionvec_init_page(page, LIONVEC_PAGE_REFINE);
		off = PageAddItem(page, (Item) it->ref, rsize, InvalidOffsetNumber,
						  false, false);
		if (off == InvalidOffsetNumber)
			elog(ERROR, "lionvec: could not add a refine item of %zu bytes", rsize);
		it->code->rblk = BufferGetBlockNumber(rbuf);
		it->code->roff = off;
		if (rnew)
			meta.refine_tail = BufferGetBlockNumber(rbuf);
	}

	page = GenericXLogRegisterBuffer(state, cbuf, 0);
	if (BufferIsValid(nbuf))
	{
		LionVecPageGetOpaque(page)->next = BufferGetBlockNumber(nbuf);
		page = GenericXLogRegisterBuffer(state, nbuf, GENERIC_XLOG_FULL_IMAGE);
		lionvec_init_page(page, LIONVEC_PAGE_CODE);
		meta.code_tail = BufferGetBlockNumber(nbuf);
	}
	if (PageAddItem(page, (Item) it->code, it->codesize, InvalidOffsetNumber,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "lionvec: could not add an item of %zu bytes", it->codesize);
	lionvec_note_key(page, &it->code->tid);

	page = GenericXLogRegisterBuffer(state, metabuf, 0);
	LionVecPageGetMeta(page)->code_tail = meta.code_tail;
	LionVecPageGetMeta(page)->refine_tail = meta.refine_tail;
	GenericXLogFinish(state);

	if (BufferIsValid(nbuf))
		UnlockReleaseBuffer(nbuf);
	UnlockReleaseBuffer(cbuf);
	if (BufferIsValid(rbuf))
		UnlockReleaseBuffer(rbuf);
	UnlockReleaseBuffer(metabuf);
}

static bool
lionvecinsert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
			  Relation heapRel, IndexUniqueCheck checkUnique,
			  bool indexUnchanged, IndexInfo *indexInfo)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "lionvec insert",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	LionVecInfo *info = lionvec_get_info(index);
	LionVecItems it;

	if (!isnull[0] && info->dims == 0)
	{
		Pointer		tofree;
		int			dim;

		(void) lionvec_datum_floats(values[0], &dim, &tofree);
		lionvec_set_dims(index, dim);
		info = lionvec_get_info(index);
	}

	lionvec_items_alloc(&it, info->dimsp);
	lionvec_items_fill(info, &it, ht_ctid, values[0], isnull[0]);

	/* a serializable reader holds the index's relation lock (§44.5) */
	CheckForSerializableConflictIn(index, NULL, InvalidBlockNumber);
	lionvec_append(index, &it, info->dimsp);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return false;
}

/* ----------------------------------------------------------------
 * VACUUM
 * ----------------------------------------------------------------
 */

typedef struct LionVecRefineRef
{
	BlockNumber blk;
	OffsetNumber off;
	ItemPointerData tid;
} LionVecRefineRef;

static int
lionvec_ref_cmp(const void *a, const void *b)
{
	const LionVecRefineRef *ra = (const LionVecRefineRef *) a;
	const LionVecRefineRef *rb = (const LionVecRefineRef *) b;

	if (ra->blk != rb->blk)
		return ra->blk < rb->blk ? -1 : 1;
	if (ra->off != rb->off)
		return ra->off < rb->off ? -1 : 1;
	return 0;
}

/*
 * Remove the REFINE items of rows whose CODE items are gone, a page at a
 * time in block order.  Each is checked by TID before it is removed; one
 * that is not there - a crash between the two steps of an earlier VACUUM -
 * is left alone.  The line pointer stays (unused), so that the offsets the
 * other rows' CODE items hold stay right.
 */
static void
lionvec_vacuum_refines(Relation index, LionVecRefineRef *refs, int nrefs,
					   int dimsp, BufferAccessStrategy strategy)
{
	int			i = 0;

	if (nrefs == 0)
		return;
	qsort(refs, nrefs, sizeof(LionVecRefineRef), lionvec_ref_cmp);
	while (i < nrefs)
	{
		BlockNumber blk = refs[i].blk;
		Buffer		buf;
		Page		page;
		GenericXLogState *state;
		int			ndel = 0;

		lion_vacuum_delay_point();
		buf = ReadBufferExtended(index, MAIN_FORKNUM, blk, RBM_NORMAL, strategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		lionvec_check_page(index, BufferGetPage(buf), blk, LIONVEC_PAGE_REFINE);
		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		for (; i < nrefs && refs[i].blk == blk; i++)
		{
			if (lionvec_refine_at(index, page, blk, refs[i].off, dimsp,
								  &refs[i].tid) == NULL)
				continue;
			PageIndexTupleDeleteNoCompact(page, refs[i].off);
			ndel++;
		}
		if (ndel > 0)
			GenericXLogFinish(state);
		else
			GenericXLogAbort(state);
		UnlockReleaseBuffer(buf);
	}
}

static IndexBulkDeleteResult *
lionvecbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	LionVecMetaData meta;
	BlockNumber blk;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint64		visited = 0;
	LionVecRefineRef *refs;
	int			nrefs = 0;
	int			maxrefs;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	lionvec_read_meta(index, &meta);

	/* the REFINE items to remove, in maintenance_work_mem, flushed when full */
	maxrefs = (int) Min((Size) maintenance_work_mem * 1024 / sizeof(LionVecRefineRef),
						MaxAllocSize / sizeof(LionVecRefineRef));
	maxrefs = Max(maxrefs, 2 * MaxOffsetNumber);
	refs = palloc(sizeof(LionVecRefineRef) * maxrefs);

	for (blk = meta.code_head; BlockNumberIsValid(blk);)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber dels[MaxOffsetNumber];
		int			ndel = 0;
		OffsetNumber off;
		OffsetNumber maxoff;
		BlockNumber next;

		lion_vacuum_delay_point();
		lionvec_chain_step(index, &visited, &nblocks);
		buf = ReadBufferExtended(index, MAIN_FORKNUM, blk, RBM_NORMAL,
								 info->strategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		lionvec_check_page(index, page, blk, LIONVEC_PAGE_CODE);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off = OffsetNumberNext(off))
		{
			LionVecCode *code = lionvec_code_at(index, page, blk, off, meta.dimsp);

			if (callback(&code->tid, callback_state))
			{
				dels[ndel++] = off;
				if (BlockNumberIsValid(code->rblk))
				{
					refs[nrefs].blk = code->rblk;
					refs[nrefs].off = code->roff;
					refs[nrefs].tid = code->tid;
					nrefs++;
				}
			}
			else
				stats->num_index_tuples += 1;
		}
		if (ndel > 0)
		{
			GenericXLogState *state = GenericXLogStart(index);

			page = GenericXLogRegisterBuffer(state, buf, 0);
			PageIndexMultiDelete(page, dels, ndel);
			GenericXLogFinish(state);
			stats->tuples_removed += ndel;
		}
		next = LionVecPageGetOpaque(BufferGetPage(buf))->next;
		UnlockReleaseBuffer(buf);

		/* room for the next page's, or flush */
		if (nrefs > maxrefs - MaxOffsetNumber)
		{
			lionvec_vacuum_refines(index, refs, nrefs, meta.dimsp, info->strategy);
			nrefs = 0;
		}
		blk = next;
	}
	lionvec_vacuum_refines(index, refs, nrefs, meta.dimsp, info->strategy);
	pfree(refs);

	stats->num_pages = RelationGetNumberOfBlocks(index);
	return stats;
}

static IndexBulkDeleteResult *
lionvecvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	/* nothing to clean up beyond what ambulkdelete did, nor to count without it */
	if (info->analyze_only || stats == NULL)
		return stats;
	stats->num_pages = RelationGetNumberOfBlocks(info->index);
	return stats;
}

/* ----------------------------------------------------------------
 * Costs (§44.7)
 * ----------------------------------------------------------------
 */

/* a row's plane-1 estimate, in cpu_operator_costs: the popcounts and a sqrt */
#define LIONVEC_ROW_OPS			2.0

/*
 * Pages of the CODE chain of an index of `pages` pages: the CODE items'
 * share of the space, by the dimensions the meta page records (or 1536 if
 * none yet).
 */
static double
lionvec_code_pages(Oid indexoid, double pages, double *perpage,
				   double *dirpages)
{
	Relation	index = index_open(indexoid, NoLock);
	LionVecMetaData meta;
	double		code;
	double		ref;
	int			dimsp;

	lionvec_read_meta(index, &meta);
	index_close(index, NoLock);
	*dirpages = ceil((double) meta.nbuilt / LIONVEC_DIR_PER_PAGE);
	dimsp = meta.dims > 0 ? meta.dimsp : 1536;
	code = MAXALIGN(LIONVEC_CODE_SIZE(dimsp)) + sizeof(ItemIdData);
	ref = MAXALIGN(LIONVEC_REFINE_SIZE(dimsp)) + sizeof(ItemIdData);
	*perpage = Max(1.0, floor((BLCKSZ - SizeOfPageHeaderData -
							   MAXALIGN(sizeof(LionVecPageOpaqueData))) / code));
	return Max(1.0, ceil(pages * code / (code + ref)));
}

/*
 * The cost of a scan's first row, which is most of its cost: the pass over
 * plane 1 of every row (or, restricted, the directory and every page with a
 * member), then a batch's refines and exact distances, each a page read and
 * a heap fetch.  `members` is how many rows a restriction admits, or the
 * index's rows.
 */
static Cost
lionvec_startup_cost(IndexOptInfo *index, double members, double *codepages)
{
	double		tuples = Max(index->tuples, 1.0);
	double		perpage;
	double		dirpages;
	double		pages = lionvec_code_pages(index->indexoid, index->pages, &perpage,
										   &dirpages);
	double		frac = Min(1.0, Max(members, 0.0) / tuples);
	double		read;
	double		walked;
	double		spc_random;
	double		spc_seq;
	double		batch = Min((double) lionvec_batch, Max(members, 1.0));

	get_tablespace_page_costs(index->reltablespace, &spc_random, &spc_seq);

	/* the pages with a member, members placed independently, and their rows */
	read = pages * (1.0 - pow(1.0 - frac, perpage));
	walked = Min(tuples, read * perpage);
	if (frac < 1.0)
		read += dirpages;
	*codepages = read;
	return read * spc_seq +
		walked * cpu_index_tuple_cost * 0.1 +
		frac * tuples * LIONVEC_ROW_OPS * cpu_operator_cost +
		batch * 2.0 * (spc_random + cpu_index_tuple_cost);
}

bool
lion_vec_index(IndexOptInfo *index)
{
	return index->amcostestimate == lionveccostestimate;
}

Cost
lion_vec_scan_cost(IndexOptInfo *index, double members)
{
	double		codepages;

	return lionvec_startup_cost(index, members, &codepages);
}

static void
lionveccostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					Cost *indexStartupCost, Cost *indexTotalCost,
					Selectivity *indexSelectivity, double *indexCorrelation,
					double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	double		codepages;
	double		spc_random;
	double		spc_seq;

	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;

	/* nothing to give but an order: never a scan without one */
	if (path->indexorderbys == NIL)
	{
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost = get_float8_infinity();
		*indexPages = index->pages;
		return;
	}

	get_tablespace_page_costs(index->reltablespace, &spc_random, &spc_seq);
	*indexStartupCost = lionvec_startup_cost(index, index->tuples, &codepages);
	/* and every further row: a refine and an exact distance, at worst */
	*indexTotalCost = *indexStartupCost +
		Max(index->tuples, 1.0) * (spc_random + cpu_index_tuple_cost);
	*indexPages = codepages;
}

/* ----------------------------------------------------------------
 * Options, validation, the handler
 * ----------------------------------------------------------------
 */

static bytea *
lionvecoptions(Datum reloptions, bool validate)
{
	return (bytea *) build_reloptions(reloptions, validate,
									  lionvec_relopt_kind,
									  sizeof(LionVecOptions),
									  NULL, 0);
}

/*
 * An operator class of lionvec is one ORDER BY operator of strategy 1 (L2),
 * 2 (negative inner product) or 3 (cosine distance) over its input type, and
 * no support functions.
 */
static bool
lionvecvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	char	   *opclassname;
	CatCList   *proclist;
	CatCList   *oprlist;
	int			nops = 0;
	int			i;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);
	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opclassname = NameStr(classform->opcname);

	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));
	if (proclist->n_members > 0)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("lionvec operator class \"%s\" has support functions, and lionvec uses none",
						opclassname)));
		result = false;
	}

	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > LIONVEC_NSTRATEGIES ||
			oprform->amoppurpose != AMOP_ORDER ||
			oprform->amoplefttype != opcintype ||
			oprform->amoprighttype != opcintype ||
			oprform->amopsortfamily == InvalidOid)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("lionvec operator class \"%s\" has operator %u of strategy %d, and lionvec takes one ORDER BY operator of strategy 1, 2 or 3 over its input type",
							opclassname, oprform->amopopr, oprform->amopstrategy)));
			result = false;
		}
		nops++;
	}
	if (nops != 1)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("lionvec operator class \"%s\" has %d operators, and lionvec takes exactly one",
						opclassname, nops)));
		result = false;
	}

	ReleaseCatCacheList(oprlist);
	ReleaseCatCacheList(proclist);
	ReleaseSysCache(classtup);
	return result;
}

Datum
lionvec_handler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		.amstrategies = LIONVEC_NSTRATEGIES,
		.amsupport = 0,
		.amoptsprocnum = 0,
		.amcanorder = false,
		.amcanorderbyop = true,
#if PG_VERSION_NUM >= 180000
		.amcanhash = false,
		.amconsistentequality = false,
		.amconsistentordering = false,
#endif
		.amcanbackward = false,
		.amcanunique = false,
		.amcanmulticol = false,
		/* an ORDER BY with no WHERE at all is the whole point */
		.amoptionalkey = true,
		.amsearcharray = false,
		.amsearchnulls = false,
		.amstorage = false,
		.amclusterable = false,
		/* a reader takes a relation lock on the index (index_beginscan()) */
		.ampredlocks = false,
		.amcanparallel = false,
#if PG_VERSION_NUM >= 170000
		.amcanbuildparallel = false,
#endif
		.amcaninclude = false,
		.amusemaintenanceworkmem = false,
		.amsummarizing = false,
		.amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL,
		.amkeytype = InvalidOid,

		.ambuild = lionvecbuild,
		.ambuildempty = lionvecbuildempty,
		.aminsert = lionvecinsert,
#if PG_VERSION_NUM >= 170000
		.aminsertcleanup = NULL,
#endif
		.ambulkdelete = lionvecbulkdelete,
		.amvacuumcleanup = lionvecvacuumcleanup,
		.amcanreturn = NULL,
		.amcostestimate = lionveccostestimate,
#if PG_VERSION_NUM >= 180000
		.amgettreeheight = NULL,
#endif
		.amoptions = lionvecoptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = lionvecvalidate,
		.amadjustmembers = NULL,
		.ambeginscan = lionvecbeginscan,
		.amrescan = lionvecrescan,
		.amgettuple = lionvecgettuple,
		.amgetbitmap = NULL,
		.amendscan = lionvecendscan,
		.ammarkpos = NULL,
		.amrestrpos = NULL,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
#if PG_VERSION_NUM >= 180000
		.amtranslatestrategy = NULL,
		.amtranslatecmptype = NULL,
#endif
	};

#if PG_VERSION_NUM >= 190000
	PG_RETURN_POINTER(&amroutine);
#else
	{
		/* before 19 the caller pfree()s the result: see lion_handler() */
		IndexAmRoutine *copy = makeNode(IndexAmRoutine);

		*copy = amroutine;
		PG_RETURN_POINTER(copy);
	}
#endif
}
