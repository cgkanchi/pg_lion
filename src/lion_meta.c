/*-------------------------------------------------------------------------
 *
 * lion_meta.c
 *		The meta page: its initialization, reading it, and what VACUUM, ANALYZE
 *		and the build record on it.
 *
 * Part of the page layer, which lion.h declares.  See DESIGN.md sections 4
 * and 5.
 *
 * Every page modification in the page layer goes through the WAL shim of
 * DESIGN.md §25 (lion_wal_begin/register_buffer/op/finish), which writes
 * either a GenericXLog record or one of the extension's own: the buffer is
 * registered before it is touched and lion_wal_finish() runs before any
 * lock is dropped.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "lion.h"

/*
 * Fill in a meta page image.
 */
void
lion_init_metapage(Page page, uint32 inline_limit, BlockNumber root,
				  uint32 height, uint32 dirpages, uint32 wal_mode)
{
	LionMetaPageData *meta;

	lion_init_page(page, LION_PAGE_META);

	meta = LionPageGetMeta(page);
	memset(meta, 0, sizeof(LionMetaPageData));
	meta->magic = LION_MAGIC;
	meta->version = LION_VERSION;
	meta->offset_bits = LION_OFFSET_BITS;
	meta->container_bits = LION_CONTAINER_BITS;
	meta->unused_nbuckets = 0;
	meta->inline_limit = inline_limit;
	meta->root = root;
	meta->height = height;
	meta->dirpages = dirpages;
	meta->wal_mode = wal_mode;

	/*
	 * ... and the key counts of DESIGN.md §33 after it, none of them valid
	 * until a build records its own (lion_meta_record_ndistinct()).
	 */
	memset(LionPageGetMetaNdistinct(page), 0, sizeof(LionMetaNdistinct));
	((PageHeader) page)->pd_lower = LION_META_NDISTINCT_END;
	Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);
}

/*
 * The key counts of DESIGN.md §33 for ix's columns: the VALUE entries of each
 * scalar key column, and the rows under the first of them, as of now.
 */
void
lion_meta_fill_ndistinct(LionMetaNdistinct *nd, const LionIndexState *ix,
						 const uint64 *nvalues, uint64 rows)
{
	int			i;

	memset(nd, 0, sizeof(LionMetaNdistinct));
	for (i = 0; i < ix->ncolumns && i < LION_META_MAX_COLS; i++)
	{
		/* a multi-key column's entries are elements, not its values */
		if (ix->cols[i].multikey)
			continue;
		nd->valid_cols |= ((uint32) 1) << i;
		nd->ndistinct[i] = nvalues[i];
	}
	nd->rows = rows;
}

/*
 * Put the counts on a meta page image that nothing else can see yet: the
 * build's, which the bulk writer logs whole.
 */
void
lion_meta_record_ndistinct(Page metapage, const LionMetaNdistinct *nd)
{
	Assert(LionMetaHasNdistinct(metapage));
	memcpy(LionPageGetMetaNdistinct(metapage), nd, sizeof(LionMetaNdistinct));
}

/*
 * Write nd - or, when it is NULL, the counts the page holds already - on the
 * index's meta page, in a record of their own (LION_XLOG_META, operation
 * NDISTINCT, DESIGN.md §25 and §33), with the page's count of finished bulk
 * deletes plus bump (§37).  That number is the page's, never the caller's -
 * the counts lion_meta_fill_ndistinct() fills in carry a zero there - so that
 * no ANALYZE ever takes it back.  A meta page written before §33 gets the area
 * here: pd_lower moves to cover it, in generic mode as in rmgr mode, so that
 * neither GenericXLog's diff - which leaves out, and at redo zeroes, what lies
 * between pd_lower and pd_upper - nor a full-page image drops it.  The record
 * carries the whole area, the count with it, and redo copies it, so a standby
 * and a crash recovery end with the primary's number in both modes.
 *
 * Counts the page holds already are not written again: an ANALYZE of a table
 * nothing has changed in, or whose changes left every count and the rows as
 * they were, writes no WAL for them - nor, the first time after a checkpoint,
 * an image of the meta page.  A bump is always written.
 *
 * The caller has read the meta page through lion_get_index_state(), so the
 * index's WAL mode is known without reading it again under the lock taken here
 * (lion_index_meta_wal_mode()).
 */
static void
lion_meta_put_ndistinct(Relation index, const LionMetaNdistinct *nd,
						uint32 bump)
{
	Buffer		buf;
	Page		page;
	LionWalState *xstate;
	LionMetaNdistinct cur;
	LionMetaNdistinct put;
	bool		has;

	buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	/* the special area's size first, as lion_read_meta() checks it */
	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
		!LionPageIsMeta(page) ||
		((PageHeader) page)->pd_upper < LION_META_NDISTINCT_END)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "lion index \"%s\": block 0 is not the meta page",
			 RelationGetRelationName(index));
	}
	has = LionMetaHasNdistinct(page);
	if (has)
		memcpy(&cur, LionPageGetMetaNdistinct(page), sizeof(LionMetaNdistinct));
	else
		memset(&cur, 0, sizeof(LionMetaNdistinct));
	put = (nd != NULL) ? *nd : cur;
	put.bulkdeletes = cur.bulkdeletes + bump;
	if (has && memcmp(&cur, &put, sizeof(LionMetaNdistinct)) == 0)
	{
		UnlockReleaseBuffer(buf);
		return;
	}

	xstate = lion_wal_begin(index);
	page = lion_wal_register_buffer(xstate, buf, LION_WALBUF_STD);
	memcpy(LionPageGetMetaNdistinct(page), &put, sizeof(LionMetaNdistinct));
	if (((PageHeader) page)->pd_lower < LION_META_NDISTINCT_END)
		((PageHeader) page)->pd_lower = LION_META_NDISTINCT_END;
	lion_wal_op(xstate, page, LION_OP_NDISTINCT, 0, 0, &put,
				sizeof(LionMetaNdistinct));
	lion_wal_finish(xstate, LION_XLOG_META);

	UnlockReleaseBuffer(buf);
}

/* The counts VACUUM or ANALYZE took, with the bulk deletes kept as they are. */
void
lion_meta_write_ndistinct(Relation index, const LionMetaNdistinct *nd)
{
	lion_meta_put_ndistinct(index, nd, 0);
}

/*
 * The end of an ambulkdelete call: one more bulk delete finished (DESIGN.md
 * §37), written after the last TID the call took out, in the same record as
 * the counts it took (or, with nd NULL, the ones the page has).  A call that
 * found nothing to take out counts as well: the TIDs a VACUUM finds gone may
 * have been taken out by an earlier call that failed before its end, and
 * their heap pages are marked all-visible only after this one.
 */
void
lion_meta_count_bulkdelete(Relation index, const LionMetaNdistinct *nd)
{
	lion_meta_put_ndistinct(index, nd, 1);
}

/*
 * The counts on the index's meta page; false, and *nd zeroed, when it carries
 * none - a meta page written before DESIGN.md §33 that nothing has counted
 * since.  A bad meta page is refused as lion_read_meta() refuses it.
 */
bool
lion_read_meta_ndistinct(Relation index, LionMetaNdistinct *nd)
{
	Buffer		buf;
	Page		page;
	bool		have = false;

	memset(nd, 0, sizeof(LionMetaNdistinct));

	buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (PageIsNew(page) || PageGetSpecialSize(page) != LION_SPECIAL_SIZE ||
		!LionPageIsMeta(page))
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index))));
	}
	if (LionMetaHasNdistinct(page) &&
		((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper)
	{
		memcpy(nd, LionPageGetMetaNdistinct(page), sizeof(LionMetaNdistinct));
		have = true;
	}
	UnlockReleaseBuffer(buf);

	return have;
}

/*
 * Read and validate the meta page.
 */
void
lion_read_meta(Relation index, LionMetaPageData *meta)
{
	Buffer		buf;
	Page		page;
	LionMetaPageData *ondisk;
	uint16		special;

	buf = ReadBuffer(index, LION_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	/*
	 * The special area's SIZE is checked before anything reads it.  The
	 * buffer manager's page check accepts any pd_special up to BLCKSZ, so a
	 * damaged meta page - or one of another format, whose special area had
	 * another size - would otherwise have the opaque read past the end of the
	 * page.  The meta data itself sits right after the page header in every
	 * format, so a page of the wrong size is still read for its magic and
	 * version: an index of an older or newer format gets the message about
	 * its version below, which is the one that says what to do.
	 */
	special = PageIsNew(page) ? 0 : PageGetSpecialSize(page);
	if (PageIsNew(page) ||
		(special == LION_SPECIAL_SIZE &&
		 (!LionPageIsMeta(page) ||
		  LionPageGetOpaque(page)->page_id != LION_PAGE_ID)))
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index))));
	}

	ondisk = LionPageGetMeta(page);
	*meta = *ondisk;
	UnlockReleaseBuffer(buf);

	/*
	 * Version 6 is the base format and version 7 the same with summary posting
	 * sets (DESIGN.md §32): both are read, and a version 6 index is one whose
	 * columns have no summaries.  Anything older predates a format change that
	 * moved or reinterpreted items, and anything newer is a format this build
	 * does not know - and the hint says which of the two it is, since REINDEX
	 * with this build is the way out of either, but the reason differs.
	 */
	if (meta->magic != LION_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index)),
				 errdetail("Meta page magic %08X, expected %08X.",
						   meta->magic, LION_MAGIC)));
	if (meta->version != LION_VERSION &&
		meta->version != LION_VERSION_SUMMARIES)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index)),
				 errdetail("Meta page magic %08X version %u, expected %08X version %u or %u.",
						   meta->magic, meta->version, LION_MAGIC, LION_VERSION,
						   LION_VERSION_SUMMARIES),
				 meta->version < LION_VERSION ?
				 errhint("REINDEX the index: its on-disk format predates this build of pg_lion.") :
				 errhint("The index was written by a newer build of pg_lion than this one: use that build, or REINDEX the index with this one.")));

	/* The right version, and a special area of the wrong size: damage. */
	if (special != LION_SPECIAL_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index)),
				 errdetail("Its meta page has a special area of %u bytes, expected %u.",
						   (unsigned) special, (unsigned) LION_SPECIAL_SIZE)));

	/*
	 * The summary words (§32) must agree with the version: a version 6 meta
	 * page has zeros there, and a version 7 one names at least one column and
	 * a bucket size a build could have chosen.
	 */
	if ((meta->version == LION_VERSION) != (meta->summary_cols == 0) ||
		(meta->summary_cols != 0 &&
		 (meta->summary_tids < LION_MIN_SUMMARY_TIDS ||
		  meta->summary_tids > LION_MAX_SUMMARY_TIDS)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid lion index",
						RelationGetRelationName(index)),
				 errdetail("Meta page version %u names summarized columns %08X with buckets of %u TIDs.",
						   meta->version, meta->summary_cols,
						   meta->summary_tids)));

	if (meta->offset_bits != LION_OFFSET_BITS ||
		meta->container_bits != LION_CONTAINER_BITS)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" was built for a different block size",
						RelationGetRelationName(index)),
				 errdetail("Index has offset_bits %u, container_bits %u; this build uses %d and %d.",
						   meta->offset_bits, meta->container_bits,
						   LION_OFFSET_BITS, LION_CONTAINER_BITS)));

	if (!BlockNumberIsValid(meta->root))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has no directory root",
						RelationGetRelationName(index))));
}

/*
 * Record on a meta page image the order the build laid ix's directory out in:
 * which key columns are ordered, and by which comparisons (§21).  ix is the
 * state the build computed from the catalog, with no order recorded yet.
 */
void
lion_meta_record_order(LionMetaPageData *meta, LionIndexState *ix)
{
	int			i;

	meta->order_flags |= LION_META_ORDER_RECORDED;
	meta->ordered_cols = 0;
	for (i = 0; i < ix->ncolumns; i++)
	{
		if (ix->cols[i].ordered)
			meta->ordered_cols |= ((uint32) 1) << i;
		else if (ix->cols[i].sqlbodycmp)
			ereport(NOTICE,
					(errmsg("key column %d of lion index is laid out in hash order", i + 1),
					 errdetail("Its comparison function has a SQL-standard body, which the index cannot recognise again after a change."),
					 errhint("Give the comparison a string body or write it in C to have the column ordered.")));
	}
	meta->order_ident = lion_order_ident(ix);
}

void
lion_meta_record_summaries(LionMetaPageData *meta, uint32 cols,
						   uint32 bucket_tids)
{
	if (cols == 0)
	{
		meta->version = LION_VERSION;
		meta->summary_cols = 0;
		meta->summary_tids = 0;
	}
	else
	{
		meta->version = LION_VERSION_SUMMARIES;
		meta->summary_cols = cols;
		meta->summary_tids = bucket_tids;
	}
}
