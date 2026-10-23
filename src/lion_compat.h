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
 * old_snapshot_threshold (16 only; 17 removed the feature).  When it is set,
 * VACUUM removes rows an old snapshot can still see and marks their pages
 * all-visible, and every read of a page has to call TestForOldSnapshot() to
 * turn that into "snapshot too old" - which lion does not, so it refuses to
 * be read at all while the threshold is set (DESIGN.md §9,
 * lion_check_old_snapshot()).  It is a postmaster setting, so the answer
 * cannot change under a running server; on 17 and later it is constant false
 * and the refusals compile away.
 */
#if PG_VERSION_NUM < 170000
#define lion_old_snapshot_threshold_active()	OldSnapshotThresholdActive()
#else
#define lion_old_snapshot_threshold_active()	false
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
 * Does EXPLAIN qualify a deparsed Var with its table, as core's useprefix
 * decides it?  18 appends an RTE_GROUP entry to every GROUP BY query's range
 * table and counts the entries without it in es->rtable_size; below 18 the
 * range table's length is that count.
 */
#include "commands/explain.h"

#if PG_VERSION_NUM >= 180000
#define lion_explain_useprefix(es)	((es)->rtable_size > 1 || (es)->verbose)
#else
#define lion_explain_useprefix(es)	(list_length((es)->rtable) > 1 || (es)->verbose)
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

/* ... and is gtopr the `>` of one (DESIGN.md §36: ORDER BY count(*) DESC)? */
static inline bool
lion_ordering_op_is_gt(Oid gtopr, Oid *opfamily, Oid *opcintype)
{
#if PG_VERSION_NUM >= 180000
	CompareType cmptype;

	return get_ordering_op_properties(gtopr, opfamily, opcintype, &cmptype) &&
		cmptype == COMPARE_GT;
#else
	int16		strategy;

	return get_ordering_op_properties(gtopr, opfamily, opcintype, &strategy) &&
		strategy == BTGreaterStrategyNumber;
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
 * The next row of an index scan, into slot.  20 moved the fetch of an index
 * scan's rows into the table AM: genam.h's index_getnext_slot() is tableam.h's
 * table_index_getnext_slot(), which dispatches through the scan's
 * xs_getnext_slot; the arguments and the answer are the same.
 */
#if PG_VERSION_NUM >= 200000
#define lion_index_getnext_slot(scan, direction, slot) \
	table_index_getnext_slot(scan, direction, slot)
#else
#define lion_index_getnext_slot(scan, direction, slot) \
	index_getnext_slot(scan, direction, slot)
#endif

/*
 * The same question of many TIDs of one relation in a row, the planner's
 * endpoint probe's (DESIGN.md §28).  16-19's table_index_fetch_tuple_check()
 * creates a fetch state and a slot for every TID it is asked about and drops
 * both, and the pin on the page with them; here one of each serves all the
 * TIDs, and the pin is kept while they stay on a page, as an index scan
 * keeps it.  19 gave table_index_fetch_begin() scan options, of which this
 * passes none, as table_index_fetch_tuple_check() does.  20's
 * table_fetch_tid() creates neither, and is called as it is.
 */
typedef struct LionTidFetch
{
	Relation	rel;
#if PG_VERSION_NUM < 200000
	struct IndexFetchTableData *fetch;
	TupleTableSlot *slot;
#endif
} LionTidFetch;

static inline void
lion_tid_fetch_begin(LionTidFetch *tf, Relation rel)
{
	tf->rel = rel;
#if PG_VERSION_NUM >= 200000
#elif PG_VERSION_NUM >= 190000
	tf->slot = table_slot_create(rel, NULL);
	tf->fetch = table_index_fetch_begin(rel, SO_NONE);
#else
	tf->slot = table_slot_create(rel, NULL);
	tf->fetch = table_index_fetch_begin(rel);
#endif
}

static inline bool
lion_tid_fetch(LionTidFetch *tf, ItemPointer tid, Snapshot snapshot,
			   bool *all_dead)
{
#if PG_VERSION_NUM >= 200000
	return table_fetch_tid(tf->rel, tid, snapshot, all_dead);
#else
	bool		call_again = false;
	bool		found;

	found = table_index_fetch_tuple(tf->fetch, tid, snapshot, tf->slot,
									&call_again, all_dead);
	ExecClearTuple(tf->slot);
	return found;
#endif
}

static inline void
lion_tid_fetch_end(LionTidFetch *tf)
{
#if PG_VERSION_NUM < 200000
	table_index_fetch_end(tf->fetch);
	ExecDropSingleTupleTableSlot(tf->slot);
#endif
}

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
 * A parallel index build joins its heap scan through
 * table_beginscan_parallel() (DESIGN.md §24, "Build"), which took scan
 * options in 19; nbtree passes none, and so does lion.
 */
#if PG_VERSION_NUM >= 190000
#define lion_table_beginscan_parallel(rel, pscan) \
	table_beginscan_parallel(rel, pscan, SO_NONE)
#else
#define lion_table_beginscan_parallel(rel, pscan) \
	table_beginscan_parallel(rel, pscan)
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

/*
 * pg_always_inline is the newer spelling of pg_attribute_always_inline (which
 * every supported major has); released 16.x and 17.x minors do not define it,
 * so the count engine did not compile against them.
 */
#ifndef pg_always_inline
#define pg_always_inline pg_attribute_always_inline
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
 * A backend's fair share of shared_buffers: the pool over every process that
 * may pin a buffer of it, which is what core holds its own batch operations
 * to - a read stream, a relation extension (LimitAdditionalPins()).  18
 * exports it as GetPinLimit(); 16 and 17 compute the same number inside
 * bufmgr.c (MaxProportionalPins) and export no way to read it, so it is
 * computed here the way they compute it.  It is 0 on a pool that is small for
 * the connections it serves (DESIGN.md §15, "The pin budget").
 */
#include "storage/bufmgr.h"
#if PG_VERSION_NUM >= 180000
#define lion_pin_fair_share()	GetPinLimit()
#else
#include "miscadmin.h"
#include "storage/proc.h"
#define lion_pin_fair_share() \
	((uint32) (NBuffers / (MaxBackends + NUM_AUXILIARY_PROCS)))
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

/*
 * The snapshot a TOAST value's chunks are read under, as core's detoast reads
 * them (lionvec's locator, DESIGN.md §44.13): 18's get_toast_snapshot()
 * returns its own; 16 and 17 fill in the caller's (init_toast_snapshot()),
 * which must outlive the reads.  Either needs a registered or active
 * snapshot, as any detoast does.
 */
#include "access/toast_internals.h"

#if PG_VERSION_NUM >= 180000
#define lion_toast_snapshot(data)	((void) (data), get_toast_snapshot())
#else
static inline Snapshot
lion_toast_snapshot(SnapshotData *data)
{
	init_toast_snapshot(data);
	return data;
}
#endif

/*
 * TOAST pointers of a 4-byte value id, the only kind lionvec's locator holds
 * (DESIGN.md §44.13; LionToastPointer, lion_vec.h).  PostgreSQL 20 adds
 * TOAST relations of 8-byte ids (the toast_value_type reloption): the chunk
 * size depends on the id's type, the external size is read from va_extinfo,
 * and a pointer tagged VARTAG_ONDISK_OID8 is no value the locator can hold.
 * The macros need access/heaptoast.h and varatt.h where they are used.
 */
#if PG_VERSION_NUM >= 200000
#define LION_TOAST_MAX_CHUNK_SIZE	TOAST_OID_MAX_CHUNK_SIZE
#define LION_TOAST_EXTSIZE(p)		VARATT_EXTINFO_GET_EXTSIZE((p).va_extinfo)
#define lion_toast_ondisk_oid(attr) \
	(VARATT_IS_EXTERNAL_ONDISK(attr) && VARTAG_EXTERNAL(attr) == VARTAG_ONDISK_OID)
#else
#define LION_TOAST_MAX_CHUNK_SIZE	TOAST_MAX_CHUNK_SIZE
#define LION_TOAST_EXTSIZE(p)		VARATT_EXTERNAL_GET_EXTSIZE(p)
#define lion_toast_ondisk_oid(attr)	VARATT_IS_EXTERNAL_ONDISK(attr)
#endif

/*
 * pg_noreturn (PostgreSQL 18), written before the declaration.  Before it
 * there was pg_attribute_noreturn(), which is GCC's attribute where there is
 * one, and that may stand before the declaration too.
 */
#ifndef pg_noreturn
#define pg_noreturn pg_attribute_noreturn()
#endif

/*
 * n rows fetched by rel - an index whose order fetched them, or the table a
 * bitmap-like read fetched them from - as pgstat_count_heap_fetch() counts
 * one (pg_stat_user_indexes.idx_tup_fetch, pg_stat_user_tables.idx_tup_fetch).
 * PostgreSQL 20 keeps an index's counts apart from a table's, in a union of
 * the backend's relation status, and defines PGSTAT_KIND_INDEX for them.
 */
#include "pgstat.h"

#ifdef PGSTAT_KIND_INDEX
#define lion_pgstat_count_fetches(rel, n) \
	do { \
		if (pgstat_should_count_relation(rel)) \
		{ \
			if ((rel)->pgstat_info->kind == PGSTAT_KIND_INDEX) \
				(rel)->pgstat_info->idx.tuples_fetched += (n); \
			else \
				(rel)->pgstat_info->tab.counts.tuples_fetched += (n); \
		} \
	} while (0)
#else
#define lion_pgstat_count_fetches(rel, n) \
	do { \
		if (pgstat_should_count_relation(rel)) \
			(rel)->pgstat_info->counts.tuples_fetched += (n); \
	} while (0)
#endif

/*
 * compute_bitmap_pages()'s count of loops is a double from PostgreSQL 17, an
 * int before it; a caller gives a double, which on 16 is held to an int's
 * range.  optimizer/cost.h declares it, where it is called.
 */
#if PG_VERSION_NUM >= 170000
#define lion_compute_bitmap_pages(root, rel, qual, loops, cost, tuples) \
	compute_bitmap_pages((root), (rel), (qual), (loops), (cost), (tuples))
#else
#define lion_compute_bitmap_pages(root, rel, qual, loops, cost, tuples) \
	compute_bitmap_pages((root), (rel), (qual), \
						 (int) Min((loops), (double) PG_INT32_MAX), \
						 (cost), (tuples))
#endif

/*
 * A MemoizePath's expected rescans are est_calls from PostgreSQL 19, calls
 * before it.  19 also keeps the hit ratio cost_memoize_rescan() priced it at
 * (est_hit_ratio), which 16 to 18 leave to be worked out again.
 */
#if PG_VERSION_NUM >= 190000
#define lion_memoize_calls(mpath)	((mpath)->est_calls)
#else
#define lion_memoize_calls(mpath)	((mpath)->calls)
#endif

#endif							/* LION_COMPAT_H */
