/*-------------------------------------------------------------------------
 * lion_wal.h
 *	  The WAL shim and the custom resource manager (DESIGN.md §25).
 *
 * Every page change of a lion index goes through the four calls below.  They
 * wrap one of two loggers, chosen per INDEX from the `wal_mode` field of its
 * meta page:
 *
 *	generic	 GenericXLog: register the buffer (an 8 KB image copy), modify
 *			 the image, and let GenericXLogFinish() diff it against the page.
 *			 This is what the extension did before §25 and is what an index
 *			 built on a server that did not register the resource manager
 *			 keeps using.
 *
 *	rmgr	 our own resource manager: modify the PAGE, and log the bytes
 *			 that changed - an offset, a length and the bytes, or the
 *			 operation itself where replaying it is cheaper than the bytes it
 *			 moves.  No image copy, no diff, and a lock hold that is one
 *			 memcpy long.
 *
 * The shim keeps the storage code from forking: a call site registers its
 * buffers, works on the Page it is handed, describes what it did with
 * lion_wal_op() - which is a no-op in generic mode, where the diff describes
 * it already - and finishes.
 *
 * THE ONE RULE A CALL SITE MUST FOLLOW.  In rmgr mode lion_wal_begin() opens
 * a CRITICAL SECTION, because the page is modified in place and an ERROR
 * between the first modification and XLogInsert() would leave a page in
 * shared buffers that no WAL record describes.  So everything that can fail -
 * allocating a page, reading a buffer, anything that palloc's a lot - has to
 * happen BEFORE lion_wal_begin(), and a record that may have to be abandoned
 * must decide that before it modifies anything (lion_wal_abort() asserts it).
 * That is the same discipline core's own access methods follow, and it is why
 * allocating a page is split into lion_alloc_page(), which is fallible and
 * runs first, and lion_wal_init_buffer(), which is not.
 *
 * See DESIGN.md §25 for the record catalogue and the redo rules.
 *-------------------------------------------------------------------------
 */
#ifndef LION_WAL_H
#define LION_WAL_H

#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/xlog_internal.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

/* The name wal_consistency_checking and pg_walinspect know us by. */
#define LION_RMGR_NAME			"pg_lion"

/* Values of LionMetaPageData.wal_mode. */
#define LION_WAL_MODE_GENERIC	0
#define LION_WAL_MODE_RMGR		1

/*
 * The most buffers one record may carry.  XLogRegisterBuffer() would allow
 * more, but the lock protocol of §4, §21 and §22 was designed around the four
 * GenericXLog allows (a chain split is P, M, N and the entry leaf), and every
 * argument about lock ordering in DESIGN.md rests on that bound.
 */
#define LION_WAL_MAX_BLOCKS		4

/* ---------- record types (the top four bits of xl_info) ---------- */

#define LION_XLOG_ITEM_SET			0x00	/* in-place change inside items */
#define LION_XLOG_ITEM_REPLACE		0x10	/* PageIndexTupleOverwrite */
#define LION_XLOG_ITEM_ADD			0x20	/* PageAddItemExtended */
#define LION_XLOG_ITEM_DELETE		0x30	/* item removal (cleanup lock) */
#define LION_XLOG_PAGE_INIT			0x40	/* a page allocated by itself */
#define LION_XLOG_SPLIT				0x50	/* a page split and its siblings */
#define LION_XLOG_DOWNLINK			0x60	/* a downlink into a parent */
#define LION_XLOG_SPLIT_CLEAR		0x70	/* clear INCOMPLETE_SPLIT */
#define LION_XLOG_META				0x80	/* the meta page */
#define LION_XLOG_VACUUM_PAGE		0x90	/* VACUUM's per-page removals */
#define LION_XLOG_PAGE_DELETED		0xA0	/* a page handed to the FSM */
#define LION_XLOG_ENTRY				0xB0	/* entry add/replace/delete */

/* ---------- the per-block operation stream ---------- */

/*
 * A record's payload is, for each registered block, a packed stream of these
 * headers, each followed by `len` payload bytes.  The stream is NOT aligned,
 * so it is read with memcpy; that keeps the hot record - an in-place member
 * insert - at a few dozen bytes instead of paying MAXALIGN twice over.
 */
typedef struct xl_lion_op
{
	uint8		op;				/* LION_OP_* */
	uint8		unused;
	uint16		off;			/* OffsetNumber, op-specific */
	uint16		aux;			/* op-specific */
	uint16		len;			/* payload bytes following this header */
} xl_lion_op;

#define SizeOfLionOp			((Size) sizeof(xl_lion_op))

StaticAssertDecl(sizeof(xl_lion_op) == 8, "xl_lion_op must be 8 bytes");

#define LION_OP_INIT		1	/* re-init: payload = LionPageOpaqueData */
#define LION_OP_SPECIAL		2	/* payload = LionPageOpaqueData, page kept */
#define LION_OP_ADD			3	/* PageAddItemExtended at off */
#define LION_OP_ADDMANY		4	/* aux items from off up; {uint16 len, bytes}* */
#define LION_OP_REPLACE		5	/* PageIndexTupleOverwrite at off */
#define LION_OP_SETBYTES	6	/* memcpy into the item at off, at byte aux */
#define LION_OP_MULTIDEL	7	/* PageIndexMultiDelete, aux offsets */
#define LION_OP_DELETE_NC	8	/* PageIndexTupleDeleteNoCompact, aux offsets */
#define LION_OP_DELETE		9	/* PageIndexTupleDelete at off */
#define LION_OP_MINMAX		10	/* lion_page_update_minmax() */
#define LION_OP_FLAGS		11	/* opaque->flags = aux */
#define LION_OP_META		12	/* payload = LionMetaPageData */
#define LION_OP_DELETED		13	/* lion_page_set_deleted(), payload = xid */
#define LION_OP_CONTAINER_ADD 14	/* lion_container_add(item at off, aux) */
#define LION_OP_SPARSE_INS	15	/* lion_sparse_insert(off, payload ckey, aux) */

/*
 * The record header, which always travels in the main data so that redo can
 * read it even when every block carries a full-page image.
 */
typedef struct xl_lion_header
{
	uint8		initmask;		/* bit b: block b is initialised by this record */
	uint8		cleanupmask;	/* bit b: redo needs a CLEANUP lock on block b */
	uint16		unused;
} xl_lion_header;

#define SizeOfLionHeader		((Size) sizeof(xl_lion_header))

/* ---------- flags for lion_wal_register_buffer() ---------- */

#define LION_WALBUF_STD			0x00	/* an ordinary change to a live page */
#define LION_WALBUF_INIT		0x01	/* this record initialises the page */
#define LION_WALBUF_IMAGE		0x02	/* force a full-page image */
#define LION_WALBUF_CLEANUP		0x04	/* redo must hold a cleanup lock (§25) */

/* ---------- the shim ---------- */

typedef struct LionWalState LionWalState;

/*
 * Start a record.  In rmgr mode this enters a critical section and everything
 * fallible must already be done (see the file header).  Writing an rmgr-mode
 * index on a server that did not register the resource manager ERRORs here,
 * with the shared_preload_libraries hint.
 */
extern LionWalState *lion_wal_begin(Relation index);

/*
 * Register a buffer and get the Page to work on: the scratch image in generic
 * mode, the buffer's own page in rmgr mode.  The buffer must be held
 * EXCLUSIVE (a cleanup lock counts) until lion_wal_finish() returns.
 */
extern Page lion_wal_register_buffer(LionWalState *state, Buffer buf, int flags);

/*
 * Describe one change to a page this record registered.  A no-op in generic
 * mode, where the byte-wise diff describes it already.  `page` is the pointer
 * lion_wal_register_buffer() returned.
 */
extern void lion_wal_op(LionWalState *state, Page page, uint8 op,
						OffsetNumber off, uint16 aux,
						const void *data, Size len);

/* Append bytes to the payload of the operation most recently described. */
extern void lion_wal_op_append(LionWalState *state, Page page,
							   const void *data, Size len);

/*
 * Set the `aux` field of the operation most recently described.  An ADDMANY
 * is emitted before its items are known - a fill loop skips unused line
 * pointers - and this is how it learns how many there were.
 */
extern void lion_wal_op_count(LionWalState *state, Page page, uint16 aux);

/* Record-level payload, for what is not about one block. */
extern void lion_wal_register_data(LionWalState *state, const void *ptr, Size len);

/* Write the record (info is one of LION_XLOG_*) and leave the critical section. */
extern void lion_wal_finish(LionWalState *state, uint8 info);

/*
 * Abandon the record.  Legal only while nothing has been modified, which in
 * generic mode GenericXLogAbort() guarantees by construction and in rmgr mode
 * is asserted: the two callers that use it (a PageIndexTupleOverwrite that
 * does not fit) decide before they write.
 */
extern void lion_wal_abort(LionWalState *state);

/*
 * VACUUM's REGROW window (DESIGN.md §11 and §25).
 *
 * A container that grew while it was being filtered is re-placed through the
 * general machinery of lion_pages.c - the same code an INSERT goes down - and
 * that machinery has no idea that the item it is writing has had TIDs taken
 * out of it.  It matters, because a record that removes a TID has to replay
 * under a CLEANUP lock on the page it removes it from, and nothing else about
 * those records says so.
 *
 * Between these two calls, every record marks its FIRST registered block -
 * which for every one of those paths is the container page being written - as
 * needing a cleanup lock at redo.  It travels here rather than through five
 * function signatures because it is a property of the CALLER (VACUUM holds a
 * cleanup lock on that page throughout) and not of the operation.
 */
extern void lion_wal_removal_begin(void);
extern void lion_wal_removal_end(void);

/* The wal_mode of an index, from its (cached) meta page. */
extern int lion_wal_mode(Relation index);

/* Was the resource manager registered in this server? */
extern bool lion_rmgr_registered(void);

/* Register it; called from _PG_init while preloading (DESIGN.md §25). */
extern void lion_wal_init(void);

/* The GUC behind pg_lion.rmgr_id, exposed for the stats/verify surface. */
extern PGDLLIMPORT int lion_rmgr_id;

/*
 * The wal_mode a CREATE INDEX gives a new index: the reloption when it was
 * set, else rmgr when this server has the resource manager and generic when
 * it has not.  ERRORs when the reloption asks for rmgr and it is not there.
 */
extern uint32 lion_wal_mode_for_build(Relation index);

#endif							/* LION_WAL_H */
