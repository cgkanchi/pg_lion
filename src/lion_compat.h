/*-------------------------------------------------------------------------
 * lion_compat.h
 *	  The differences between the PostgreSQL major versions pg_lion builds
 *	  against (16 and later), in one place.
 *
 *	  Everything else in the extension is written against the newest API and
 *	  includes this file to get it on older servers.  Each shim names the
 *	  release that introduced what it stands in for, so that dropping a major
 *	  version is a matter of deleting the blocks that mention it.
 *-------------------------------------------------------------------------
 */
#ifndef LION_COMPAT_H
#define LION_COMPAT_H

#include "postgres.h"

#if PG_VERSION_NUM < 160000
#error "pg_lion requires PostgreSQL 16 or later"
#endif

#include "access/stratnum.h"
#include "access/tableam.h"
#include "access/tupdesc.h"
#include "catalog/pg_opfamily.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * Injection points (PostgreSQL 17; the second, `arg` parameter is 18's).
 * The call sites use LION_INJECTION_POINT(name) and pass no argument, so on
 * 17 the one-argument form serves, and on 16 - or on any server built without
 * --enable-injection-points - it is nothing at all, which is what core's own
 * macro is in that case too.
 */
#if PG_VERSION_NUM >= 170000
#include "utils/injection_point.h"
#endif

#if PG_VERSION_NUM >= 180000
#define LION_INJECTION_POINT(name)	INJECTION_POINT(name, NULL)
#elif PG_VERSION_NUM >= 170000
#define LION_INJECTION_POINT(name)	INJECTION_POINT(name)
#else
#define LION_INJECTION_POINT(name)	((void) 0)
#endif

/*
 * The snapshots a plain index scan may drop its pins under (DESIGN.md §29.5),
 * as nbtree decides it: 19 narrowed IsMVCCSnapshot() to regular MVCC
 * snapshots and named the old meaning, historic ones included,
 * IsMVCCLikeSnapshot().
 */
#include "utils/snapmgr.h"
#ifdef IsMVCCLikeSnapshot
#define LION_IS_MVCC_LIKE(snapshot)	IsMVCCLikeSnapshot(snapshot)
#else
#define LION_IS_MVCC_LIKE(snapshot)	IsMVCCSnapshot(snapshot)
#endif

/*
 * RestrictSearchPath() (17) sets search_path to "pg_catalog, pg_temp" for the
 * current GUC nest level; 17's maintenance commands - CREATE INDEX, REINDEX,
 * VACUUM, amcheck - evaluate the table owner's index expressions under it.
 * 16's evaluate them under the session's search_path, and so does
 * lion_index_verify() there, which is what makes it evaluate an expression
 * exactly as that server's own CREATE INDEX did.
 */
#include "utils/guc.h"

#if PG_VERSION_NUM < 170000
#define RestrictSearchPath()	((void) 0)
#endif

/* vacuum_delay_point() took is_analyze in 18. */
#if PG_VERSION_NUM >= 180000
#define lion_vacuum_delay_point()	vacuum_delay_point(false)
#else
#define lion_vacuum_delay_point()	vacuum_delay_point()
#endif

/*
 * Is ltopr the `<` of a btree ordering, and of which family and input type?
 * 18 turned the strategy number get_ordering_op_properties() reports into a
 * CompareType.
 */
static inline bool
lion_ordering_op_is_lt(Oid ltopr, Oid *opfamily, Oid *opcintype)
{
#if PG_VERSION_NUM >= 180000
	CompareType cmptype;

	return get_ordering_op_properties(ltopr, opfamily, opcintype, &cmptype) &&
		cmptype == COMPARE_LT;
#else
	int16		strategy;

	return get_ordering_op_properties(ltopr, opfamily, opcintype, &strategy) &&
		strategy == BTLessStrategyNumber;
#endif
}

/*
 * Does any version of the HOT chain rooted at tid satisfy snapshot?  20 renamed
 * table_index_fetch_tuple_check() to table_fetch_tid() when the index-fetch
 * callbacks moved into the table AM; the arguments and the answer are the same.
 */
#if PG_VERSION_NUM >= 200000
#define lion_table_fetch_tid(rel, tid, snapshot, all_dead) \
	table_fetch_tid(rel, tid, snapshot, all_dead)
#else
#define lion_table_fetch_tid(rel, tid, snapshot, all_dead) \
	table_index_fetch_tuple_check(rel, tid, snapshot, all_dead)
#endif

/*
 * On-access pruning of a heap page the count's recheck is about to read
 * (DESIGN.md §11, "On-access pruning sets the visibility map too").  19 gave
 * heap_page_prune_opt() a visibility map pin and a rel_read_only flag, with
 * which it marks the page all-visible when what is left after pruning is;
 * that is the whole reason the count calls it.  16-18 have the two-argument
 * form, which only prunes - core's own scans do that already, so the count
 * leaves it to them and this is nothing at all there.
 *
 * The contract is core's: buf pinned and NOT locked (the function takes the
 * cleanup lock itself, conditionally), *vmbuf the caller's reusable pin.
 */
#if PG_VERSION_NUM >= 190000
#include "access/heapam.h"
#define lion_heap_page_prune_opt(rel, buf, vmbuf, rel_read_only) \
	heap_page_prune_opt(rel, buf, vmbuf, rel_read_only)
#else
#define lion_heap_page_prune_opt(rel, buf, vmbuf, rel_read_only) \
	((void) (rel), (void) (buf), (void) (vmbuf), (void) (rel_read_only))
#endif

/*
 * The range table indexes a statement modifies or row-locks - what
 * ScanRelIsReadOnly() (19) tests a scan's relation against.  19 keeps them as
 * PlannedStmt.resultRelationRelids and .rowMarkRelids; before, they are the
 * resultRelations list and the rti of each PlanRowMark.
 */
#include "nodes/plannodes.h"

static inline Bitmapset *
lion_pstmt_written_rtis(const PlannedStmt *pstmt)
{
#if PG_VERSION_NUM >= 190000
	return bms_union(pstmt->resultRelationRelids, pstmt->rowMarkRelids);
#else
	Bitmapset  *rtis = NULL;
	ListCell   *lc;

	foreach(lc, pstmt->resultRelations)
		rtis = bms_add_member(rtis, lfirst_int(lc));
	foreach(lc, pstmt->rowMarks)
		rtis = bms_add_member(rtis, (int) ((PlanRowMark *) lfirst(lc))->rti);
	return rtis;
#endif
}

/*
 * A syscache callback's cache argument is a SysCacheIdentifier since 19, an
 * int before.
 */
#if PG_VERSION_NUM >= 190000
typedef SysCacheIdentifier LionSysCacheId;
#else
typedef int LionSysCacheId;
#endif

/* 19 requires TupleDescFinalize() on a hand-built descriptor; before, nothing. */
#if PG_VERSION_NUM < 190000
#define TupleDescFinalize(tupdesc)	((void) (tupdesc))
#endif

/* get_opfamily_name() moved into lsyscache.c in 18. */
#if PG_VERSION_NUM < 180000
static inline char *
get_opfamily_name(Oid opfid, bool missing_ok)
{
	HeapTuple	tup;
	char	   *result;

	tup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfid));
	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for operator family %u", opfid);
		return NULL;
	}
	result = pstrdup(NameStr(((Form_pg_opfamily) GETSTRUCT(tup))->opfname));
	ReleaseSysCache(tup);
	return result;
}
#endif

/*
 * 18 made the data arguments of the WAL registration functions `const void *`
 * and 19 did the same to the page item functions, which took `char *` (Item)
 * before; the call sites are written for the newer signatures.  A macro of
 * the function's own name is not expanded again inside its expansion, so this
 * only adds the cast.
 */
#include "access/xloginsert.h"
#include "storage/bufpage.h"

#if PG_VERSION_NUM < 180000
#define XLogRegisterData(data, len) \
	XLogRegisterData((char *) (data), len)
#define XLogRegisterBufData(block_id, data, len) \
	XLogRegisterBufData(block_id, (char *) (data), len)
#endif

#if PG_VERSION_NUM < 190000
#define PageAddItemExtended(page, item, size, offnum, flags) \
	PageAddItemExtended(page, (Item) (item), size, offnum, flags)
#define PageIndexTupleOverwrite(page, offnum, newtup, newsize) \
	PageIndexTupleOverwrite(page, offnum, (Item) (newtup), newsize)
#endif

/*
 * The bulk-write API (storage/bulk_write.h) is 17's.  On 16 the same five
 * calls are provided by lion_build.c on top of smgrextend()/smgrwrite() and
 * log_newpage(), the way 16's own nbtree build writes its pages: each page is
 * checksummed and WAL-logged as it is handed over, blocks past the current
 * end are reached by zero-extending, and the relation is synced at the end
 * unless it is temporary.
 */
#if PG_VERSION_NUM >= 170000
#include "storage/bulk_write.h"
#else
typedef PGIOAlignedBlock *BulkWriteBuffer;
typedef struct BulkWriteState BulkWriteState;

extern BulkWriteState *smgr_bulk_start_rel(Relation rel, ForkNumber forknum);
extern BulkWriteBuffer smgr_bulk_get_buf(BulkWriteState *bulkstate);
extern void smgr_bulk_write(BulkWriteState *bulkstate, BlockNumber blocknum,
							BulkWriteBuffer buf, bool page_std);
extern void smgr_bulk_finish(BulkWriteState *bulkstate);
#endif

#endif							/* LION_COMPAT_H */
