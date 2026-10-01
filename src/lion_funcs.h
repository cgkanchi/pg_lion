/*-------------------------------------------------------------------------
 *
 * lion_funcs.h
 *		SQL-callable helpers for the lion index (DESIGN.md section 7):
 *		lion_index_stats(), lion_index_verify(), lion_index_posting_root()
 *		and lion_index_wal_mode().
 *
 * lion_index_stats() opens the index with AccessShareLock and reads one page
 * at a time under a SHARE lock, so it runs concurrently with INSERT and
 * VACUUM.  Its counters are sums over pages read at different moments, which
 * is all a statistic promises, and it checks no more of a page than it needs
 * to read the page safely: telling damage apart is lion_index_verify()'s job.
 *
 * lion_index_verify() is a PARENT check in amcheck's sense
 * (bt_index_parent_check()): it compares every level of the directory and of
 * each posting tree with the whole level below it, and proves that every
 * block is reachable exactly once.  It does that WITHOUT BLOCKING WRITERS,
 * the way CREATE INDEX CONCURRENTLY builds an index beside them (DESIGN.md
 * section 7):
 *
 *	- It takes ShareUpdateExclusiveLock on the table and then on the index.
 *	  INSERT, UPDATE and DELETE go on; VACUUM, ANALYZE, DDL and a second
 *	  verify() wait.  With VACUUM out nothing leaves the index while it is
 *	  walked - no TID, no entry, no page - so the only changes it can meet
 *	  are the ones an INSERT makes, and each of them is either invisible to a
 *	  check or accounted for below.
 *	- Every check that a concurrent INSERT can make fail on a sound index is
 *	  settled on the spot where one more read proves it (a right sibling's
 *	  left link after a split, a root flag after a root split, a link past
 *	  the block count taken at the start), retried where a whole posting set
 *	  has to be read again, and otherwise recorded as a CANDIDATE: a downlink
 *	  to a page the walk of its level did not reach, a live page no walk
 *	  reached.  After the walk, and only when there are candidates, it waits
 *	  for the statements that are writing the index - WaitForLockers() on the
 *	  index, in the mode CIC waits in, which conflicts with the
 *	  RowExclusiveLock a writing statement holds on it - and checks the
 *	  candidates again, one targeted lookup each.  What is still wrong then is
 *	  reported.  The wait lasts as long as those statements do, unless
 *	  lock_timeout or statement_timeout ends it first; an index nobody wrote
 *	  to while it was walked is not waited for at all.
 *	- A posting set is walked without its entry's directory leaf held, and
 *	  the entry is read again afterwards: every change a writer makes to a
 *	  set moves its counters or its tail, and every writer of a key holds
 *	  that leaf from its first record to its last, so an entry that reads the
 *	  same before and after was walked while nobody wrote to it.  A set that
 *	  keeps changing is walked a last time with the leaf held SHARE, which
 *	  keeps the writers of that one leaf's keys waiting for that one walk.
 *
 * During recovery no lock above RowExclusiveLock can be taken and replay
 * takes no relation locks at all: there it takes AccessShareLock, reports
 * what it finds at once, as it always has, and is exact only while replay
 * leaves the index alone.  None of the settling above holds against replay,
 * which locks each record's pages for that record alone, so a standby is the
 * one place verify() can report damage that is not there.  What it must
 * never do there is hold a directory page while it waits for a posting page:
 * replay takes a record's posting pages BEFORE its directory leaf and holds
 * both to the end of the record (DESIGN.md §25), and two buffer locks taken
 * in opposite orders are a deadlock nothing detects and nothing cancels.  So
 * a standby walks a posting set as a primary first does - with nothing held,
 * the entry read again afterwards - and a set that replay keeps changing is
 * kept from its last walk without comparing its totals, never walked with
 * the leaf held; and heapallindexed lets the entry's leaf go before it
 * descends the set, on a primary too.
 *
 * With heapallindexed, lion_index_verify() evaluates the index's expressions
 * and predicate, which are the table owner's code; it runs them as the table
 * owner, in a security-restricted operation, exactly as amcheck does since
 * CVE-2022-1552.
 *
 * This header is private to the SQL-callable helpers: lion_funcs.c and
 * the lion_verify*.c files of lion_index_verify().  What they share is
 * declared here, and nothing else includes it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_FUNCS_H
#define LION_FUNCS_H


#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "varatt.h"

#include "lion.h"
#include "lion_count.h"


#define LION_STATS_NCOLS		30

/*
 * A check a concurrent INSERT can make fail on a sound index, recorded instead
 * of reported and settled once the writers that were in flight are done
 * (DESIGN.md §7, "suspect, wait, recheck").
 */
typedef enum LionVerifyCandKind
{
	/*
	 * A downlink of the directory to a page the walk of the level below did
	 * not reach: a split made that page after the walk had passed its left
	 * neighbour.  Settled by walking the level from that neighbour to the
	 * next page the walk did reach, which is where a split puts it.
	 */
	LION_VCAND_DOWNLINK,

	/*
	 * A live page no walk reached: one a split, a root push-down or a spill
	 * made - from the end of the relation or out of the free space map -
	 * after the walk had passed the place it went.  Settled by a search for
	 * the page's own first key from its tree's root, or, for the root of a
	 * posting set, by finding the entry that names it.
	 */
	LION_VCAND_UNREACHED
} LionVerifyCandKind;

typedef struct LionVerifyCand
{
	LionVerifyCandKind kind;
	BlockNumber blk;
	uint16		level;			/* DOWNLINK: the level blk has to be on */
	BlockNumber left;			/* DOWNLINK: the reached page before it */
	BlockNumber right;			/* DOWNLINK: the reached page after it, or
								 * InvalidBlockNumber when there is none */
	int			downlink;		/* DOWNLINK: its position on its level */
} LionVerifyCand;

typedef struct LionVerifyState
{
	Relation	index;
	Relation	heap;
	LionIndexState *ix;
	BlockNumber nblocks;		/* the block count, re-read when a link passes it */
	BlockNumber startblocks;	/* ... and what it was when the check began */
	uint8	   *refs;			/* how often each block is referenced */
	LionContainer *cbuf;			/* aligned container work buffer */
	BlockNumber root;			/* the directory root, from the meta page */
	uint32		height;
	/* per key column (DESIGN.md §24): at most one of each, per column */
	int64		nnullentries[INDEX_MAX_KEYS];
	int64		nemptyentries[INDEX_MAX_KEYS];
	int64		nlastsummaries[INDEX_MAX_KEYS];	/* §32: at most one */
	uint16		lastattno;		/* attno of the last leaf entry seen */
	uint32		max_posting_height;	/* tallest posting tree (DESIGN.md §22) */
	Oid			keyoutfunc;		/* output function of the column being checked */
	MemoryContext heapcxt;		/* per heap tuple, heapallindexed only */
	int64		nheaptuples;
	bool		showvalues;		/* the CALLER is a superuser; decided before
								 * the switch to the table owner */

	/*
	 * Writers may run beside the check: true on a primary, where the check
	 * holds ShareUpdateExclusiveLock.  On a standby it is false, and every
	 * suspicion is reported the moment it arises, as it always was there -
	 * but a posting set is walked with nothing held there too, and walked
	 * again when replay changed it (lion_verify_set()).
	 */
	bool		concurrent;
	bool		rootsplit;		/* the meta page showed a taller directory */
	LionVerifyCand *cands;		/* what the walk could not settle */
	int			ncands;
	int			maxcands;

	/*
	 * The blocks the posting-set walk in progress has marked in refs, so that
	 * a walk that has to be repeated (lion_verify_set()) can unmark them: a
	 * page reached twice is otherwise a page with two owners.
	 */
	MemoryContext setcxt;		/* for one set's walks, emptied after it */
	bool		settrack;
	BlockNumber *setvisits;		/* allocated outside setcxt */
	int			nsetvisits;
	int			maxsetvisits;

	/* What the concurrency cost, for the DEBUG1 line at the end. */
	int64		nleftlinks;		/* left links settled by a coupled walk */
	int64		nrootsplits;	/* root flags settled by the meta page */
	int64		nsetwalks;		/* posting-set walks */
	int64		nsetretries;	/* ... thrown away because a writer got in */
	int64		nsetholds;		/* ... made with the entry's leaf held */
	int64		nsetuncompared;	/* sets a standby kept without their totals */
	int			nwaits;			/* WaitForLockers() calls */
} LionVerifyState;

/*
 * How many times a posting set is walked with nothing held before the walk is
 * made with its entry's directory leaf held SHARE (lion_verify_set()).  Each
 * extra walk costs this backend a read of the set; the last one costs the
 * writers of that leaf's keys a wait for one read of it.
 */
#define LION_VERIFY_SET_ATTEMPTS	3

/*
 * How many walks of a posting set a STANDBY makes before it gives up on
 * getting to the end of one (lion_verify_set()).  It never walks a set with
 * the entry's leaf held - replay takes the set's pages before the leaf
 * (DESIGN.md §25) - so after LION_VERIFY_SET_ATTEMPTS walks it keeps the next
 * one that reaches the end of the set, and only a root pushed down or an
 * upper level split under each walk keeps it from doing that.
 */
#define LION_VERIFY_STANDBY_WALKS	10

/* What one walk of a posting set found (lion_verify_chain()). */
typedef struct LionVerifySetResult
{
	uint64		card;			/* TIDs its leaves hold */
	uint32		ncontainers;	/* items its leaves hold */
	BlockNumber last;			/* the last leaf of the right-link chain */
	uint32		height;			/* its root's level */
	int			nincomplete;	/* pages flagged LION_PAGE_INCOMPLETE_SPLIT */
	int			maxincomplete;
	BlockNumber *incomplete;
} LionVerifySetResult;

/*
 * Report structural damage.  Every message names the block (and item) the
 * problem was found in, as the caller has no other way of locating it.
 */
#define lion_corrupt(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg(__VA_ARGS__)))

/*
 * The same, for damage that nothing but a rebuild repairs and that an earlier
 * version is known to have made: the order of the summaries of DESIGN.md §32.
 */
#define lion_corrupt_reindex(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg(__VA_ARGS__), \
			 errhint("REINDEX the index.")))

/*
 * What an unreferenced block is, as far as the reachability pass cares.
 */
typedef enum LionVerifyUnref
{
	LION_UNREF_FREE,			/* a DELETED page: free, and in the map */
	LION_UNREF_LEAK,			/* what an interrupted allocation leaves */
	LION_UNREF_INTERNAL,		/* an internal posting page */
	LION_UNREF_LIVE,			/* a live page, which has to be reachable */
	LION_UNREF_FOREIGN			/* not a page of this index at all */
} LionVerifyUnref;
/* A macro, so that the compiler sees the ERROR does not return. */
#define lion_verify_unreachable(vs, blk) \
	lion_corrupt("lion index \"%s\": block %u is not reachable from the meta page", \
				RelationGetRelationName((vs)->index), (blk))

/* Defined in one of the SQL helper function files and used in another. */

/* lion_funcs.c */
extern Relation lion_open_index(Oid relid, LOCKMODE lockmode);

/* lion_verify.c */
extern void lion_verify_refresh_nblocks(LionVerifyState *vs);
extern bool lion_verify_block_exists(LionVerifyState *vs, BlockNumber blk);
extern void lion_verify_visit(LionVerifyState *vs, BlockNumber blk,
							  const char *what);
extern void lion_verify_add_cand(LionVerifyState *vs,
								 const LionVerifyCand *cand);
extern int lion_verify_blkcmp(const void *a, const void *b);
extern BlockNumber *lion_verify_sorted_blocks(const BlockNumber *blocks,
											  int n);
extern bool lion_verify_has_block(const BlockNumber *sorted, int n,
								  BlockNumber blk);
extern Page lion_verify_read_page(LionVerifyState *vs, BlockNumber blk,
								  uint16 kind, Buffer *bufp);
extern ItemId lion_verify_itemid(LionVerifyState *vs, BlockNumber blk,
								 Page page, OffsetNumber off);
extern LionEntryTuple *lion_verify_dir_item(LionVerifyState *vs,
											BlockNumber blk, Page page,
											OffsetNumber off);
extern void lion_verify_entry(LionVerifyState *vs, BlockNumber blk,
							  OffsetNumber off, ItemId iid, Page page);

/* lion_verify_dir.c */
extern LionEntryTuple *lion_verify_copy_item(Page page, OffsetNumber off);
extern void lion_verify_directory(LionVerifyState *vs);
extern void lion_verify_meta(LionVerifyState *vs);
extern LionVerifyUnref lion_verify_classify(LionVerifyState *vs,
											BlockNumber blk);
extern void lion_verify_warn_leak(LionVerifyState *vs, BlockNumber blk);
extern void lion_verify_reachable(LionVerifyState *vs);

/* lion_verify_heap.c */
extern void lion_verify_wait_for_writers(LionVerifyState *vs);
extern void lion_verify_recheck(LionVerifyState *vs);
extern void lion_verify_heapallindexed(LionVerifyState *vs);

/* lion_verify_summary.c */
extern void lion_verify_summaries(LionVerifyState *vs);

#endif							/* LION_FUNCS_H */
