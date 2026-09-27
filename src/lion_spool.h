/*-------------------------------------------------------------------------
 *
 * lion_spool.h
 *	  The build's input: every key column's codes grouped by key in memory,
 *	  spilled in runs when memory runs out, and merged back out in directory
 *	  order (DESIGN.md §24, "Build").  lion_build.c drives it and writes the
 *	  pages; nothing here touches the index relation.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_SPOOL_H
#define LION_SPOOL_H

#include "storage/itemptr.h"

#include "lion.h"

/* One process's accumulator for every key column of one index. */
typedef struct LionSpool LionSpool;

/*
 * What the merge hands the writer: the entries of one directory position, and
 * their codes.  A group is ONE entry except when an unordered opclass's hash
 * collides, and then it is every distinct key of that hash, in directory
 * order; lion_spool_group_next() gives the codes of all of them in ascending
 * code order and says whose each one is, which is the order the sort-based
 * build handed them to its builders in (DESIGN.md §21, "A hash COLLISION").
 */
typedef struct LionSpoolGroup LionSpoolGroup;

typedef struct LionSpoolEntry
{
	int			kind;			/* LION_KIND_NULL, _EMPTY or _VALUE */
	uint32		hash;			/* LION_NULLKEY_HASH for a reserved kind */
	Datum		key;			/* lion_fetch_key() of raw; 0 when reserved */
	const char *raw;			/* the key's stored bytes (lion_store_key()) */
	Size		rawlen;
} LionSpoolEntry;

typedef void (*LionSpoolEmit) (void *arg, LionSpoolGroup *group);

extern int	lion_spool_group_size(const LionSpoolGroup *group);
extern const LionSpoolEntry *lion_spool_group_entry(const LionSpoolGroup *group,
													int i);
extern bool lion_spool_group_next(LionSpoolGroup *group, int *entry,
								  uint64 *code);

/* membytes is what the accumulators of all the columns may use together. */
extern LionSpool *lion_spool_begin(LionIndexState *ix, Size membytes);
extern void lion_spool_add(LionSpool *spool, ItemPointer tid, Datum *values,
						   bool *isnull);
extern double lion_spool_ntids(const LionSpool *spool);
extern int	lion_spool_nruns(const LionSpool *spool);

/* One column's entries, in directory order, to emit. */
extern void lion_spool_emit_column(LionSpool *spool, int col,
								   LionSpoolEmit emit, void *arg);

extern void lion_spool_end(LionSpool *spool);

#endif							/* LION_SPOOL_H */
