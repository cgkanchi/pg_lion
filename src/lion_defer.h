/*-------------------------------------------------------------------------
 *
 * lion_defer.h
 *		Statement-level insert batching (DESIGN.md §55).
 *
 * A statement that inserts many rows hands each of them to aminsert, which
 * keeps the (key, TID) pairs of the eligible columns in a buffer per index
 * instead of writing them; they go into the index sorted by key, each key's
 * rows under one hold of its directory leaf, at the end of the statement
 * (aminsertcleanup) or before anything in the backend reads the index.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_DEFER_H
#define LION_DEFER_H

#include "nodes/execnodes.h"
#include "utils/relcache.h"

#include "lion.h"

/*
 * One (key, TID) pair waiting for its index: the key as lion_key_prepare()
 * made it, copied into the buffer, and the bucket hash the directory files it
 * under, or a reserved entry's flag with key 0 and hash LION_NULLKEY_HASH.
 */
typedef struct LionInsertRow
{
	Datum		key;
	uint64		code;			/* lion_tid_to_code() of the heap TID */
	uint32		hash;
	uint16		flag;			/* 0, or the reserved entry's flag */
	uint16		col;			/* 0-based key column */
} LionInsertRow;

typedef struct LionDefer LionDefer;

extern PGDLLIMPORT bool lion_batch_inserts;	/* pg_lion.batch_inserts */

extern void lion_defer_init(void);
extern bool lion_defer_allowed(IndexInfo *indexInfo);
extern LionDefer *lion_defer_get(Relation index, Relation heap);
extern void lion_defer_add(LionDefer *d, Relation index, Relation heap,
						   LionState *state, int col, Datum key,
						   uint16 flag, uint64 code);
extern void lion_defer_flush_index(Relation index);
extern void lion_defer_flush_heap(Oid heapoid);
extern void lion_defer_flush_all(void);
extern void lioninsertcleanup(Relation index, IndexInfo *indexInfo);

/* lion_insert.c: write a buffer's rows, sorting them first */
extern void lion_insert_apply(Relation index, Relation heaprel,
							  LionInsertRow *rows, int nrows);

#endif							/* LION_DEFER_H */
