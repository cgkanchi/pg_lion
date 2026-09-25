/*-------------------------------------------------------------------------
 *
 * lion_funcs.c
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
 * block is reachable exactly once.  None of that holds between pages read at
 * different moments while writers split them - a level walked before a split
 * and its parent walked after it disagree, a block number taken at the start
 * is exceeded by the first page a split allocates, and a page allocated after
 * the walk passed is reachable from nothing it saw - so it takes ShareLock on
 * the table and then on the index, which keeps every INSERT, UPDATE, DELETE
 * and VACUUM out for as long as it runs and makes it check one index rather
 * than a moving one.  During recovery no lock above RowExclusiveLock can be
 * taken, and no lock stops replay anyway: there it takes AccessShareLock and
 * is exact only while replay leaves the index alone (DESIGN.md section 7).
 *
 * With heapallindexed, lion_index_verify() evaluates the index's expressions
 * and predicate, which are the table owner's code; it runs them as the table
 * owner, in a security-restricted operation, exactly as amcheck does since
 * CVE-2022-1552.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

#include "lion.h"

PG_FUNCTION_INFO_V1(lion_index_stats);
PG_FUNCTION_INFO_V1(lion_index_verify);
PG_FUNCTION_INFO_V1(lion_index_posting_root);
PG_FUNCTION_INFO_V1(lion_index_wal_mode);

#define LION_STATS_NCOLS		24

typedef struct LionVerifyState
{
	Relation	index;
	Relation	heap;
	LionIndexState *ix;
	BlockNumber nblocks;
	uint8	   *refs;			/* how often each block is referenced */
	LionContainer *cbuf;			/* aligned container work buffer */
	BlockNumber root;			/* the directory root, from the meta page */
	uint32		height;
	/* per key column (DESIGN.md §24): at most one of each, per column */
	int64		nnullentries[INDEX_MAX_KEYS];
	int64		nemptyentries[INDEX_MAX_KEYS];
	uint16		lastattno;		/* attno of the last leaf entry seen */
	uint32		max_posting_height;	/* tallest posting tree (DESIGN.md §22) */
	Oid			keyoutfunc;		/* output function of the column being checked */
	MemoryContext heapcxt;		/* per heap tuple, heapallindexed only */
	int64		nheaptuples;
	bool		showvalues;		/* the CALLER is a superuser; decided before
								 * the switch to the table owner */
} LionVerifyState;

/*
 * Report structural damage.  Every message names the block (and item) the
 * problem was found in, as the caller has no other way of locating it.
 */
#define lion_corrupt(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg(__VA_ARGS__)))

/*
 * Open relid as a lion index.
 */
static Relation
lion_open_index(Oid relid, LOCKMODE lockmode)
{
	Relation	index = index_open(relid, lockmode);

	if (index->rd_rel->relkind != RELKIND_INDEX ||
		index->rd_indam == NULL ||
		index->rd_indam->ambuild != lionbuild)
	{
		char	   *name = pstrdup(RelationGetRelationName(index));

		index_close(index, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a lion index", name)));
	}

	if (RELATION_IS_OTHER_TEMP(index))
	{
		char	   *name = pstrdup(RelationGetRelationName(index));

		index_close(index, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary index \"%s\" of another session",
						name)));
	}

	return index;
}

/* ---------------------------------------------------------------------
 * lion_index_stats()
 *
 * One row per KEY COLUMN (DESIGN.md §24).  Counters that describe an entry or
 * a posting set are that column's own; counters that describe the RELATION -
 * the directory's shape, the free and deleted pages - are the index's and are
 * repeated on every row, because a directory leaf holds the entries of
 * whatever columns happen to land on it and there is nothing to divide.
 *
 * A container page IS attributable: its owner_head is the head block of the
 * entry that owns it, and entries carry their column.  The two are met in
 * whichever order the block walk finds them, so a multicolumn index
 * accumulates per posting set in a hash keyed by the head block and adds the
 * totals up at the end.  A single-column index skips all of that: everything
 * it holds belongs to column 1.
 * --------------------------------------------------------------------- */

/* Per-column counters. */
typedef struct LionColStats
{
	int64		entries;
	int64		inline_entries;
	int64		containers;
	int64		by_type[4];		/* indexed by LionContainerType */
	int64		sparse_segments;	/* items of type LION_CT_SPARSE */
	int64		sparse_members; /* (ckey, lo) pairs inside them */
	int64		ntids;
	int64		null_tids;		/* members of the reserved NULL entry (§14) */
	int64		empty_tids;		/* members of the reserved EMPTY entry (§17) */
	int64		container_pages;	/* posting-tree LEAVES (DESIGN.md §22) */
	int64		posting_internal_pages; /* ... and the pages above them */
	int64		container_bytes;	/* logical bytes of every item */
	int64		slack_bytes;	/* free bytes INSIDE items (DESIGN.md §4) */
	int64		inline_slack_bytes; /* ... and inside INLINE payloads (§4) */
} LionColStats;

/* What one posting set contributed, before its column is known. */
typedef struct LionSetStats
{
	BlockNumber head;			/* hash key: the set's root block */
	uint16		attno;			/* 0 until the owning entry is seen */
	LionColStats st;
} LionSetStats;

typedef struct LionStats
{
	uint32		height;			/* directory height (DESIGN.md §21) */
	int64		leaf_pages;			/* directory leaves */
	int64		internal_pages;
	int32		max_posting_height;	/* the tallest posting tree */
	int64		free_bytes;
	int64		deleted_pages;	/* freed pages awaiting reuse (DESIGN.md §18) */

	int			ncolumns;
	LionColStats *cols;			/* [ncolumns] */
	bool	   *ordered;		/* [ncolumns]: the column's own opclass (§21) */
	HTAB	   *sets;			/* head block -> LionSetStats, or NULL */
} LionStats;

/*
 * Where one container page's counters go: straight into its column for a
 * single-column index, into the per-set bucket otherwise.
 */
static LionColStats *
lion_stats_bucket(LionStats *st, BlockNumber head)
{
	LionSetStats *ent;
	bool		found;

	if (st->sets == NULL)
		return &st->cols[0];

	ent = (LionSetStats *) hash_search(st->sets, &head, HASH_ENTER, &found);
	if (!found)
	{
		ent->attno = 0;
		memset(&ent->st, 0, sizeof(LionColStats));
	}
	return &ent->st;
}

/* ... and the same for an entry, whose column is known right away. */
static void
lion_stats_claim(LionStats *st, BlockNumber head, uint16 attno)
{
	LionSetStats *ent;
	bool		found;

	if (st->sets == NULL || !BlockNumberIsValid(head))
		return;

	ent = (LionSetStats *) hash_search(st->sets, &head, HASH_ENTER, &found);
	if (!found)
	{
		ent->attno = 0;
		memset(&ent->st, 0, sizeof(LionColStats));
	}
	ent->attno = attno;
}

/*
 * Can this page be read at all?  lion_index_stats() reports damage to nobody -
 * telling it apart is lion_index_verify()'s job - but it must not read past a
 * page because of it.  A page read from disk had its header checked by
 * PageIsVerified(); one damaged in shared buffers did not, and pd_lower bounds
 * the line pointer array that every loop below walks.  A page that fails is
 * skipped, as a new one is.
 */
static bool
lion_stats_page_readable(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	return !PageIsNew(page) &&
		phdr->pd_lower >= SizeOfPageHeaderData &&
		phdr->pd_lower <= phdr->pd_upper &&
		phdr->pd_upper <= phdr->pd_special &&
		phdr->pd_special <= BLCKSZ &&
		PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
		LionPageGetOpaque(page)->page_id == LION_PAGE_ID;
}

/*
 * ... and this item, of which the reader looks at the first minlen bytes?  It
 * must lie wholly inside the page's item space; one that does not is left
 * out of the counts.
 */
static bool
lion_stats_item_readable(Page page, ItemId iid, Size minlen)
{
	PageHeader	phdr = (PageHeader) page;

	return ItemIdIsNormal(iid) &&
		ItemIdGetLength(iid) >= minlen &&
		ItemIdGetOffset(iid) >= phdr->pd_upper &&
		ItemIdGetOffset(iid) + ItemIdGetLength(iid) <= phdr->pd_special;
}

/*
 * Account for one item of a posting set.  The container counters count real
 * containers only; a sparse segment (DESIGN.md §13) is reported by
 * sparse_segments/sparse_members instead.  container_bytes is the bytes of
 * every item, whatever its kind.
 */
static void
lion_stats_item(LionColStats *cs, const LionContainer *c, Size itemlen)
{
	Size		size = lion_item_size(c);

	if (c->type == LION_CT_SPARSE)
	{
		cs->sparse_segments++;
		cs->sparse_members += (int64) c->cardinality;
	}
	else
	{
		cs->containers++;
		if (c->type >= LION_CT_ARRAY && c->type <= LION_CT_RUN)
			cs->by_type[c->type]++;
	}

	/*
	 * container_bytes counts what the items really hold; an item on a
	 * container page may have been allotted more than that, and those spare
	 * bytes - growth slack an insert can add a member into without moving
	 * anything else (DESIGN.md §4) - are reported separately.  An item inside
	 * an INLINE payload never has any.
	 */
	cs->container_bytes += (int64) size;
	if (itemlen > size)
		cs->slack_bytes += (int64) (itemlen - size);
}

Datum
lion_index_stats(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	FuncCallContext *funcctx;
	LionStats  *st;
	int			call;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;
		TupleDesc	tupdesc;
		Relation	index;
		LionIndexState *ix;
		LionContainer *cbuf;
		BlockNumber nblocks;
		BlockNumber blk;
		uint32		height = 0;
		int			i;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		index = lion_open_index(relid, AccessShareLock);
		ix = lion_get_index_state(index);

		st = (LionStats *) palloc0(sizeof(LionStats));
		st->ncolumns = ix->ncolumns;
		st->cols = (LionColStats *) palloc0(sizeof(LionColStats) *
											st->ncolumns);
		st->ordered = (bool *) palloc0(sizeof(bool) * st->ncolumns);
		for (i = 0; i < st->ncolumns; i++)
			st->ordered[i] = ix->cols[i].ordered;
		st->sets = NULL;
		if (st->ncolumns > 1)
		{
			HASHCTL		ctl;

			ctl.keysize = sizeof(BlockNumber);
			ctl.entrysize = sizeof(LionSetStats);
			ctl.hcxt = CurrentMemoryContext;
			st->sets = hash_create("lion index stats posting sets", 256, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		}

		cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);

		nblocks = RelationGetNumberOfBlocks(index);
		for (blk = 1; blk < nblocks; blk++)
		{
			Buffer		buf;
			Page		page;
			OffsetNumber maxoff;
			OffsetNumber off;

			buf = ReadBuffer(index, blk);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);

			if (!lion_stats_page_readable(page))
			{
				UnlockReleaseBuffer(buf);
				continue;
			}

			maxoff = PageGetMaxOffsetNumber(page);

			if (LionPageIsDir(page))
			{
				st->internal_pages++;
				st->free_bytes += (int64) PageGetFreeSpace(page);
			}
			else if (LionPageIsBucket(page))
			{
				st->leaf_pages++;
				st->free_bytes += (int64) PageGetFreeSpace(page);

				for (off = lion_page_first_data(page); off <= maxoff; off++)
				{
					ItemId		iid = PageGetItemId(page, off);
					LionEntryTuple *entry;
					LionColStats *cs;

					if (!lion_stats_item_readable(page, iid, LION_ENTRY_HDRSZ))
						continue;

					/*
					 * Corrupt entries are left out; verify() is what reports
					 * them.  The payload length is the item's length less
					 * the key, which must not underflow.
					 */
					entry = (LionEntryTuple *) PageGetItem(page, iid);
					if (entry->attno < 1 || entry->attno > st->ncolumns ||
						ItemIdGetLength(iid) < LionEntryPayloadOffset(entry))
						continue;
					cs = &st->cols[entry->attno - 1];

					cs->entries++;
					cs->ntids += (int64) entry->ntids;
					if (LionEntryIsNullKey(entry))
						cs->null_tids += (int64) entry->ntids;
					if (LionEntryIsEmptyKey(entry))
						cs->empty_tids += (int64) entry->ntids;

					if ((entry->flags & LION_ENTRY_INLINE) != 0)
					{
						Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry,
																   ItemIdGetLength(iid));
						Size		cur = 0;
						Size		csize;

						cs->inline_entries++;
						while ((csize = lion_inline_fetch(LionEntryGetPayload(entry),
														 paylen, &cur, cbuf)) > 0)
							lion_stats_item(cs, cbuf, csize);

						/*
						 * What the payload does not use is growth slack: the
						 * zeroed tail an insert leaves so that the next member
						 * fits without rewriting the entry, or the one VACUUM
						 * leaves when it writes a filtered payload back into
						 * the bytes the entry already had (DESIGN.md §4, §18).
						 * It is reported apart from an item's own slack
						 * because it is an entry's, not an item's.
						 */
						cs->inline_slack_bytes += (int64) (paylen - cur);
					}
					else
						lion_stats_claim(st, entry->head, entry->attno);
				}
			}
			else if (LionPageIsContainer(page))
			{
				LionColStats *cs;

				/*
				 * A DELETED page (DESIGN.md §18) holds nothing and is waiting
				 * in the free space map to be handed out again; it is neither
				 * a container page nor free space of one.
				 */
				if (LionPageIsDeleted(page))
				{
					st->deleted_pages++;
					UnlockReleaseBuffer(buf);
					continue;
				}

				st->free_bytes += (int64) PageGetFreeSpace(page);
				cs = lion_stats_bucket(st, LionPageGetOpaque(page)->owner_head);

				/*
				 * An INTERNAL posting page (DESIGN.md §22) holds downlinks,
				 * not containers.  It is counted on its own so that
				 * container_pages still means "pages that hold a posting
				 * set's items", and the tallest tree is the largest level any
				 * page claims - a set that fits one page has height 0.
				 */
				if (LionPageIsPostingInternal(page))
				{
					cs->posting_internal_pages++;
					if ((int32) LionPageGetOpaque(page)->level >
						st->max_posting_height)
						st->max_posting_height =
							(int32) LionPageGetOpaque(page)->level;
					UnlockReleaseBuffer(buf);
					continue;
				}

				cs->container_pages++;

				for (off = FirstOffsetNumber; off <= maxoff; off++)
				{
					ItemId		iid = PageGetItemId(page, off);
					LionContainer *c;

					/*
					 * lion_item_size() reads the header, and a RUN's count
					 * of runs after it, and has no size for a type it does
					 * not know; no real item is shorter than that.
					 */
					if (!lion_stats_item_readable(page, iid,
												  LION_CONTAINER_HDRSZ + sizeof(uint16)))
						continue;
					c = (LionContainer *) PageGetItem(page, iid);
					if (c->type < LION_CT_ARRAY || c->type > LION_CT_SPARSE)
						continue;

					lion_stats_item(cs, c, ItemIdGetLength(iid));
				}
			}

			UnlockReleaseBuffer(buf);
			CHECK_FOR_INTERRUPTS();
		}

		pfree(cbuf);

		/* Fold each posting set's counters into the column that owns it. */
		if (st->sets != NULL)
		{
			HASH_SEQ_STATUS seq;
			LionSetStats *ent;

			hash_seq_init(&seq, st->sets);
			while ((ent = (LionSetStats *) hash_seq_search(&seq)) != NULL)
			{
				LionColStats *cs;
				int			k;

				/*
				 * attno 0 means no live entry claimed this set: a page an
				 * interrupted allocation or a crash leaked (verify() reports
				 * those).  It belongs to no column and is left out.
				 */
				if (ent->attno < 1 || ent->attno > st->ncolumns)
					continue;
				cs = &st->cols[ent->attno - 1];

				cs->containers += ent->st.containers;
				for (k = 0; k < 4; k++)
					cs->by_type[k] += ent->st.by_type[k];
				cs->sparse_segments += ent->st.sparse_segments;
				cs->sparse_members += ent->st.sparse_members;
				cs->container_pages += ent->st.container_pages;
				cs->posting_internal_pages += ent->st.posting_internal_pages;
				cs->container_bytes += ent->st.container_bytes;
				cs->slack_bytes += ent->st.slack_bytes;
			}
			hash_destroy(st->sets);
			st->sets = NULL;
		}

		(void) lion_dir_root(index, ix, &height);
		st->height = height;

		index_close(index, AccessShareLock);

		funcctx->user_fctx = (void *) st;
		funcctx->max_calls = st->ncolumns;

		MemoryContextSwitchTo(oldcxt);
	}

	funcctx = SRF_PERCALL_SETUP();
	st = (LionStats *) funcctx->user_fctx;
	call = (int) funcctx->call_cntr;

	if (call < (int) funcctx->max_calls)
	{
		LionColStats *cs = &st->cols[call];
		Datum		values[LION_STATS_NCOLS];
		bool		nulls[LION_STATS_NCOLS];
		HeapTuple	tuple;

		memset(nulls, 0, sizeof(nulls));
		values[0] = Int16GetDatum((int16) (call + 1));
		values[1] = Int32GetDatum((int32) st->height);
		values[2] = Int64GetDatum(st->leaf_pages);
		values[3] = Int64GetDatum(st->internal_pages);
		values[4] = BoolGetDatum(st->ordered[call]);
		values[5] = Int64GetDatum(cs->entries);
		values[6] = Int64GetDatum(cs->inline_entries);
		values[7] = Int64GetDatum(cs->container_pages);
		values[8] = Int64GetDatum(cs->containers);
		values[9] = Int64GetDatum(cs->by_type[LION_CT_ARRAY]);
		values[10] = Int64GetDatum(cs->by_type[LION_CT_BITSET]);
		values[11] = Int64GetDatum(cs->by_type[LION_CT_RUN]);
		values[12] = Int64GetDatum(cs->ntids);
		values[13] = Int64GetDatum(cs->container_bytes);
		values[14] = Int64GetDatum(st->free_bytes);
		values[15] = Int64GetDatum(cs->sparse_segments);
		values[16] = Int64GetDatum(cs->sparse_members);
		values[17] = Int64GetDatum(cs->null_tids);
		values[18] = Int64GetDatum(cs->empty_tids);
		values[19] = Int64GetDatum(cs->slack_bytes);
		values[20] = Int64GetDatum(st->deleted_pages);
		values[21] = Int64GetDatum(cs->posting_internal_pages);
		values[22] = Int32GetDatum(st->max_posting_height);
		values[23] = Int64GetDatum(cs->inline_slack_bytes);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/* ---------------------------------------------------------------------
 * lion_index_wal_mode()
 * --------------------------------------------------------------------- */

/*
 * How this index is WAL-logged: "generic" or "rmgr" (DESIGN.md §25).
 *
 * The mode is a property of the INDEX, fixed at CREATE INDEX from the
 * `wal_mode` reloption and whether the server had the resource manager, and
 * recorded on the meta page - so a cluster can hold both kinds and this is
 * how to tell which is which.  REINDEX is what changes it.
 */
Datum
lion_index_wal_mode(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	index;
	int			mode;

	index = lion_open_index(relid, AccessShareLock);
	mode = lion_wal_mode(index);
	index_close(index, AccessShareLock);

	PG_RETURN_TEXT_P(cstring_to_text((mode == LION_WAL_MODE_RMGR) ?
									 "rmgr" : "generic"));
}

/* ---------------------------------------------------------------------
 * lion_index_posting_root()
 * --------------------------------------------------------------------- */

/*
 * The ROOT block of one key's posting tree, or NULL when the key has no entry
 * or its posting set is still INLINE (DESIGN.md §22).
 *
 * This exists for the tests: the root block is the identity of a posting set -
 * it is the entry's `head` and the owner stamp of every page of the set - and
 * §22 requires it never to move, which is what a root split's push-down buys.
 * Nothing else in the extension needs it, and no plan depends on it.
 */
Datum
lion_index_posting_root(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Datum		key = PG_GETARG_DATUM(1);
	Oid			keytype = get_fn_expr_argtype(fcinfo->flinfo, 1);
	Relation	index;
	LionState  *state;
	Buffer		buf = InvalidBuffer;
	OffsetNumber off;
	int64		root = -1;

	index = lion_open_index(relid, AccessShareLock);
	state = lion_get_state(index);

	/*
	 * A multi-key opclass (DESIGN.md §17) stores one entry per extracted key,
	 * so its entries are not column values: the key this takes is of the
	 * column's own type - a whole tsvector - which no entry holds, and hashing
	 * and comparing it as if it were one lexeme answered for no key at all.
	 * Refused as the count functions refuse it (lion_count.c).  The errors
	 * leave the index to the abort to close: its name is still needed.
	 */
	if (state->multikey)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("key column %d of index \"%s\" has a multi-key operator class, whose entries are not column values",
						1, RelationGetRelationName(index))));

	if (OidIsValid(keytype) && keytype != index->rd_opcintype[0])
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type %s cannot be compared with index \"%s\"",
						format_type_be(keytype),
						RelationGetRelationName(index))));

	if (lion_find_entry(index, state, BUFFER_LOCK_SHARE, key,
						lion_hash_key(state, key), &buf, &off))
	{
		LionEntryTuple *entry = lion_page_entry(BufferGetPage(buf), off);

		if ((entry->flags & LION_ENTRY_CHAIN) != 0)
			root = (int64) entry->head;
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	index_close(index, AccessShareLock);

	if (root < 0)
		PG_RETURN_NULL();
	PG_RETURN_INT64(root);
}

/* ---------------------------------------------------------------------
 * lion_index_verify()
 * --------------------------------------------------------------------- */

/*
 * Is blk a block of the index?
 *
 * vs->nblocks is the count taken when the check began, and nothing can extend
 * the index under the ShareLock the check holds - except replay, during
 * recovery, where the lock keeps nothing out.  There the count is read again
 * before a block is called out of range, so that a leaf replay has just split
 * onto a new block is not reported as a link past the end.
 */
static bool
lion_verify_block_exists(LionVerifyState *vs, BlockNumber blk)
{
	if (blk < vs->nblocks)
		return true;

	if (RecoveryInProgress())
	{
		BlockNumber n = RelationGetNumberOfBlocks(vs->index);

		if (n > vs->nblocks)
		{
			vs->refs = (uint8 *) repalloc0(vs->refs, Max(vs->nblocks, 1),
										   sizeof(uint8) * n);
			vs->nblocks = n;
		}
	}

	return blk < vs->nblocks;
}

/*
 * Record that blk is referenced by something, and refuse to look at it twice
 * (which also stops a corrupt rightlink cycle from looping forever).
 */
static void
lion_verify_visit(LionVerifyState *vs, BlockNumber blk, const char *what)
{
	if (!lion_verify_block_exists(vs, blk))
		lion_corrupt("lion index \"%s\": %s points at block %u, but the index has only %u blocks",
					RelationGetRelationName(vs->index), what, blk, vs->nblocks);

	if (vs->refs[blk] != 0)
		lion_corrupt("lion index \"%s\": block %u is referenced more than once (reached again as %s)",
					RelationGetRelationName(vs->index), blk, what);

	vs->refs[blk]++;
}

/*
 * Read blk with a SHARE lock and check its page header and kind.
 */
static Page
lion_verify_read_page(LionVerifyState *vs, BlockNumber blk, uint16 kind,
					 Buffer *bufp)
{
	Buffer		buf;
	Page		page;
	PageHeader	phdr;
	LionPageOpaque opaque;
	uint16		flags;

	/*
	 * Every block number that reaches this function was read off a page, and
	 * ReadBuffer() must not see one past the end: on 16 to 18 it treats
	 * InvalidBlockNumber as P_NEW and EXTENDS the relation, and a check that
	 * writes to what it checks is worse than one that crashes.
	 */
	if (!lion_verify_block_exists(vs, blk))
		lion_corrupt("lion index \"%s\": a link points at block %u, but the index has only %u blocks",
					RelationGetRelationName(vs->index), blk, vs->nblocks);

	buf = ReadBuffer(vs->index, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	*bufp = buf;

	if (PageIsNew(page))
		lion_corrupt("lion index \"%s\": block %u has never been initialised",
					RelationGetRelationName(vs->index), blk);

	/*
	 * The header bounds everything else on the page: the line pointer array
	 * ends at pd_lower, the items live in [pd_upper, pd_special).  A page read
	 * from disk had this checked by PageIsVerified(); one that was damaged in
	 * shared buffers did not.
	 */
	phdr = (PageHeader) page;
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
		lion_corrupt("lion index \"%s\": block %u has a page header with lower %u, upper %u and special %u",
					RelationGetRelationName(vs->index), blk,
					phdr->pd_lower, phdr->pd_upper, phdr->pd_special);

	if (PageGetSpecialSize(page) != LION_SPECIAL_SIZE)
		lion_corrupt("lion index \"%s\": block %u has a special area of %u bytes, expected %zu",
					RelationGetRelationName(vs->index), blk,
					(unsigned) PageGetSpecialSize(page),
					(Size) LION_SPECIAL_SIZE);

	opaque = LionPageGetOpaque(page);

	if (opaque->page_id != LION_PAGE_ID)
		lion_corrupt("lion index \"%s\": block %u has page id 0x%04X, expected 0x%04X",
					RelationGetRelationName(vs->index), blk,
					opaque->page_id, LION_PAGE_ID);

	flags = opaque->flags & LION_PAGE_KINDS;
	if (flags != LION_PAGE_META && flags != LION_PAGE_BUCKET &&
		flags != LION_PAGE_CONTAINER && flags != LION_PAGE_DIR)
		lion_corrupt("lion index \"%s\": block %u has flags 0x%04X, expected exactly one page kind",
					RelationGetRelationName(vs->index), blk, opaque->flags);

	/* Only a container page may ever carry the DELETED bit (DESIGN.md §18). */
	if ((opaque->flags & LION_PAGE_DELETED) != 0 && flags != LION_PAGE_CONTAINER)
		lion_corrupt("lion index \"%s\": block %u is marked deleted but is a %s page",
					RelationGetRelationName(vs->index), blk,
					flags == LION_PAGE_META ? "meta" :
					flags == LION_PAGE_DIR ? "directory" : "leaf");

	if (flags != kind)
		lion_corrupt("lion index \"%s\": block %u is a %s page, expected a %s page",
					RelationGetRelationName(vs->index), blk,
					flags == LION_PAGE_META ? "meta" :
					flags == LION_PAGE_BUCKET ? "leaf" :
					flags == LION_PAGE_DIR ? "directory" : "container",
					kind == LION_PAGE_META ? "meta" :
					kind == LION_PAGE_BUCKET ? "leaf" :
					kind == LION_PAGE_DIR ? "directory" : "container");

	return page;
}

/*
 * The line pointer of item off, checked before anything reads the item it
 * points at, which a corrupt one could otherwise place anywhere on the page
 * or past its end (amcheck's PageGetItemIdCareful()).
 *
 * An UNUSED line pointer is handed back as it is, for the caller to skip or
 * refuse: VACUUM leaves them on directory leaves, whose entry offsets never
 * move (DESIGN.md §18).  lion never marks an item dead or redirects one, and
 * a used item has to lie wholly inside the item space [pd_upper, pd_special)
 * at a MAXALIGNed offset - the same test bufpage.c applies before it moves
 * one.  The header bounds were checked by lion_verify_read_page().
 */
static ItemId
lion_verify_itemid(LionVerifyState *vs, BlockNumber blk, Page page,
				   OffsetNumber off)
{
	PageHeader	phdr = (PageHeader) page;
	ItemId		iid = PageGetItemId(page, off);
	unsigned	lpoff;
	unsigned	lplen;

	if (!ItemIdIsUsed(iid))
		return iid;

	if (!ItemIdIsNormal(iid))
		lion_corrupt("lion index \"%s\": line pointer %u on block %u is %s, which lion never makes",
					RelationGetRelationName(vs->index), off, blk,
					ItemIdIsDead(iid) ? "dead" : "a redirect");

	lpoff = ItemIdGetOffset(iid);
	lplen = ItemIdGetLength(iid);
	if (lplen == 0 || lpoff < phdr->pd_upper ||
		lpoff + lplen > phdr->pd_special || lpoff != MAXALIGN(lpoff))
		lion_corrupt("lion index \"%s\": line pointer %u on block %u points at %u bytes at offset %u, outside the item space %u .. %u",
					RelationGetRelationName(vs->index), off, blk, lplen, lpoff,
					phdr->pd_upper, phdr->pd_special);

	return iid;
}

/*
 * Check that the stored key of an entry has the length its type calls for,
 * so that hashing it cannot run off the end of the item.
 */
static void
lion_verify_keylen(LionVerifyState *vs, LionState *state, BlockNumber blk,
				  OffsetNumber off, const LionEntryTuple *entry)
{
	Size		keylen = entry->keylen;
	const char *key = LionEntryGetKey(entry);

	if (keylen == 0 || keylen > LION_MAX_KEY_SIZE)
		lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected 1 .. %d",
					RelationGetRelationName(vs->index), off, blk, keylen,
					LION_MAX_KEY_SIZE);

	if (state->typbyval)
	{
		if (keylen != sizeof(Datum))
			lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected %zu for a by-value type",
						RelationGetRelationName(vs->index), off, blk, keylen,
						sizeof(Datum));
	}
	else if (state->typlen > 0)
	{
		if (keylen != (Size) state->typlen)
			lion_corrupt("lion index \"%s\": entry %u on block %u has key length %zu, expected %d",
						RelationGetRelationName(vs->index), off, blk, keylen,
						state->typlen);
	}
	else if (state->typlen == -1)
	{
		if (VARATT_IS_EXTERNAL(key) || VARATT_IS_COMPRESSED(key) ||
			VARSIZE_ANY(key) != keylen)
			lion_corrupt("lion index \"%s\": entry %u on block %u has a malformed varlena key of %zu bytes",
						RelationGetRelationName(vs->index), off, blk, keylen);
	}
	else
	{
		if (strnlen(key, keylen) != keylen - 1)
			lion_corrupt("lion index \"%s\": entry %u on block %u has a malformed cstring key of %zu bytes",
						RelationGetRelationName(vs->index), off, blk, keylen);
	}
}

/*
 * The parts of a directory item - an entry, a high key or a downlink - that
 * the rest of the check READS before it could check them: a line pointer that
 * stays on the page, a whole header, a key column the index has, and key
 * bytes that lie inside the item and have the length the column's key type
 * calls for.  The order checks compare keys through the opclass, the
 * duplicate check calls its equality, the entry check hashes the key, and
 * lion_column() of a column the index does not have indexes past its array:
 * each of those would read wherever a lying header sent it.  So every item of
 * a directory page goes through this before any of them looks at it.
 *
 * Returns NULL for an unused line pointer, which the caller skips.
 */
static LionEntryTuple *
lion_verify_dir_item(LionVerifyState *vs, BlockNumber blk, Page page,
					 OffsetNumber off)
{
	ItemId		iid = lion_verify_itemid(vs, blk, page, off);
	LionEntryTuple *item;
	Size		itemsz;
	int			kind;

	if (!ItemIdIsUsed(iid))
		return NULL;

	itemsz = ItemIdGetLength(iid);
	if (itemsz < LION_ENTRY_HDRSZ)
		lion_corrupt("lion index \"%s\": item %u on block %u is only %zu bytes, less than an entry header",
					RelationGetRelationName(vs->index), off, blk, itemsz);

	item = (LionEntryTuple *) PageGetItem(page, iid);
	if (itemsz < LionEntryPayloadOffset(item))
		lion_corrupt("lion index \"%s\": item %u on block %u is %zu bytes, too small for its %u byte key",
					RelationGetRelationName(vs->index), off, blk, itemsz,
					item->keylen);

	kind = lion_entry_kind(item);

	/* The minus-infinity downlink has no column and no key (§21, §24). */
	if (kind == LION_KIND_MINF)
	{
		if (item->attno != 0 || item->keylen != 0)
			lion_corrupt("lion index \"%s\": the minus-infinity item %u on block %u has key column %u and a key of %u bytes",
						RelationGetRelationName(vs->index), off, blk,
						item->attno, item->keylen);
		return item;
	}

	if (item->attno < 1 || item->attno > vs->ix->ncolumns)
		lion_corrupt("lion index \"%s\": entry %u on block %u belongs to key column %u, but the index has %d",
					RelationGetRelationName(vs->index), off, blk, item->attno,
					vs->ix->ncolumns);

	if (kind != LION_KIND_VALUE)
	{
		/* the NULL and EMPTY entries, or pivots made from them (§14, §17) */
		if (item->keylen != 0)
			lion_corrupt("lion index \"%s\": %s item %u on block %u has a key of %u bytes",
						RelationGetRelationName(vs->index),
						kind == LION_KIND_NULL ? "null" : "empty", off, blk,
						item->keylen);
		return item;
	}

	lion_verify_keylen(vs, lion_column(vs->ix, (AttrNumber) item->attno),
					   blk, off, item);
	return item;
}

/*
 * An item may be allotted MORE bytes on a container page than its header
 * needs: growth slack the insert path adds a member into without moving
 * anything else on the page (DESIGN.md §4).  So the rule is not "the item
 * fills its space exactly" any more, but "it fills it to within one slack
 * allowance": avail is the allocated length (ItemIdGetLength, or the exact
 * size of an item inside an INLINE payload, which never has slack).
 */
static void
lion_verify_item_slack(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
					  const LionContainer *item, Size avail, const char *what)
{
	Size		size = lion_item_size(item);

	if (avail < size)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes but needs %zu",
					RelationGetRelationName(vs->index), what, off, blk, avail,
					size);

	if (avail > (Size) LION_CONTAINER_MAX_SIZE)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes, more than an item may ever take",
					RelationGetRelationName(vs->index), what, off, blk, avail);

	if (avail - size > LION_ITEM_SLACK_BOUND)
		lion_corrupt("lion index \"%s\": %s %u on block %u occupies %zu bytes, %zu more than its %zu bytes need (at most %d bytes of slack)",
					RelationGetRelationName(vs->index), what, off, blk, avail,
					avail - size, size, LION_ITEM_SLACK_BOUND);
}

/*
 * Check one sparse segment (DESIGN.md §13).
 *
 * Besides its own structure, a segment has to respect the two rules that
 * make the rest of the index able to ignore it: its range must start above
 * everything before it (the caller keeps the last ckey of the previous item
 * in *prevckey, so this also proves the ranges do not overlap or interleave,
 * within a page and across a chain), and no container key inside it may have
 * reached LION_SPARSE_THRESHOLD members, because such a key belongs in a
 * container of its own.
 */
static void
lion_verify_segment(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
				   const LionContainer *c, Size avail, bool *haveprev,
				   uint32 *prevckey)
{
	const char *detail = NULL;
	const uint32 *ckeys;
	uint32		n = c->cardinality;
	uint32		run = 1;
	uint32		i;

	if (!lion_sparse_check(c, avail, &detail))
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u is corrupt: %s",
					RelationGetRelationName(vs->index), off, blk, detail);

	lion_verify_item_slack(vs, blk, off, c, avail, "sparse segment");

	if (n == 0)
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u is empty",
					RelationGetRelationName(vs->index), off, blk);

	if (*haveprev && c->ckey <= *prevckey)
		lion_corrupt("lion index \"%s\": sparse segment %u on block %u starts at container key %u, not above the previous key %u",
					RelationGetRelationName(vs->index), off, blk, c->ckey,
					*prevckey);

	ckeys = LION_SPARSE_CKEYS_CONST(c);
	for (i = 1; i <= n; i++)
	{
		if (i < n && ckeys[i] == ckeys[i - 1])
		{
			run++;
			continue;
		}
		if (run >= LION_SPARSE_THRESHOLD)
			lion_corrupt("lion index \"%s\": sparse segment %u on block %u holds %u members of container key %u, which needs a container of its own",
						RelationGetRelationName(vs->index), off, blk, run,
						ckeys[i - 1]);
		run = 1;
	}

	*prevckey = lion_item_last_ckey(c);
	*haveprev = true;
}

/*
 * Check one item that is on a container page or inside an INLINE payload:
 * a container, or a sparse segment.
 */
static void
lion_verify_container(LionVerifyState *vs, BlockNumber blk, OffsetNumber off,
					 const LionContainer *c, Size avail, bool *haveprev,
					 uint32 *prevckey)
{
	const char *detail = NULL;

	/* the header has to be there before its type can say what follows */
	if (avail < LION_CONTAINER_HDRSZ)
		lion_corrupt("lion index \"%s\": item %u on block %u is only %zu bytes, less than an item header",
					RelationGetRelationName(vs->index), off, blk, avail);

	if (c->type == LION_CT_SPARSE)
	{
		lion_verify_segment(vs, blk, off, c, avail, haveprev, prevckey);
		return;
	}

	if (!lion_container_check(c, avail, &detail))
		lion_corrupt("lion index \"%s\": container %u on block %u is corrupt: %s",
					RelationGetRelationName(vs->index), off, blk, detail);

	lion_verify_item_slack(vs, blk, off, c, avail, "container");

	if (c->cardinality == 0)
		lion_corrupt("lion index \"%s\": container %u on block %u is empty",
					RelationGetRelationName(vs->index), off, blk);

	if (*haveprev && c->ckey <= *prevckey)
		lion_corrupt("lion index \"%s\": container %u on block %u has container key %u, not above the previous key %u",
					RelationGetRelationName(vs->index), off, blk, c->ckey,
					*prevckey);

	*prevckey = c->ckey;
	*haveprev = true;
}

/*
 * One level of a posting tree, as the walk of that level found it
 * (DESIGN.md §22).
 */
typedef struct LionVerifyPLevel
{
	int			npages;
	int			maxpages;
	BlockNumber *blocks;
	uint32	   *firstkey;		/* first separator, or first container key */
	bool	   *hasfirst;		/* false for an empty leaf */
	uint32	   *lastkey;		/* last container key of a leaf */
	uint32	   *highkey;		/* internal pages only */
	bool	   *hashigh;
	bool	   *incomplete;	/* flagged LION_PAGE_INCOMPLETE_SPLIT */
} LionVerifyPLevel;

static void
lion_verify_plevel_add(LionVerifyPLevel *lvl, BlockNumber blk, uint32 firstkey,
					   bool hasfirst, uint32 lastkey, uint32 highkey,
					   bool hashigh, bool incomplete)
{
	if (lvl->npages >= lvl->maxpages)
	{
		bool		first = (lvl->maxpages == 0);

		lvl->maxpages = first ? 64 : lvl->maxpages * 2;
		if (first)
		{
			lvl->blocks = (BlockNumber *) palloc(sizeof(BlockNumber) * lvl->maxpages);
			lvl->firstkey = (uint32 *) palloc(sizeof(uint32) * lvl->maxpages);
			lvl->hasfirst = (bool *) palloc(sizeof(bool) * lvl->maxpages);
			lvl->lastkey = (uint32 *) palloc(sizeof(uint32) * lvl->maxpages);
			lvl->highkey = (uint32 *) palloc(sizeof(uint32) * lvl->maxpages);
			lvl->hashigh = (bool *) palloc(sizeof(bool) * lvl->maxpages);
			lvl->incomplete = (bool *) palloc(sizeof(bool) * lvl->maxpages);
		}
		else
		{
			lvl->blocks = (BlockNumber *) repalloc(lvl->blocks, sizeof(BlockNumber) * lvl->maxpages);
			lvl->firstkey = (uint32 *) repalloc(lvl->firstkey, sizeof(uint32) * lvl->maxpages);
			lvl->hasfirst = (bool *) repalloc(lvl->hasfirst, sizeof(bool) * lvl->maxpages);
			lvl->lastkey = (uint32 *) repalloc(lvl->lastkey, sizeof(uint32) * lvl->maxpages);
			lvl->highkey = (uint32 *) repalloc(lvl->highkey, sizeof(uint32) * lvl->maxpages);
			lvl->hashigh = (bool *) repalloc(lvl->hashigh, sizeof(bool) * lvl->maxpages);
			lvl->incomplete = (bool *) repalloc(lvl->incomplete, sizeof(bool) * lvl->maxpages);
		}
	}
	lvl->blocks[lvl->npages] = blk;
	lvl->firstkey[lvl->npages] = firstkey;
	lvl->hasfirst[lvl->npages] = hasfirst;
	lvl->lastkey[lvl->npages] = lastkey;
	lvl->highkey[lvl->npages] = highkey;
	lvl->hashigh[lvl->npages] = hashigh;
	lvl->incomplete[lvl->npages] = incomplete;
	lvl->npages++;
}

/* The page kind and owner checks every page of a posting set goes through. */
static Page
lion_verify_posting_page(LionVerifyState *vs, BlockNumber blk,
						 BlockNumber eblk, OffsetNumber eoff,
						 const LionEntryTuple *entry, uint16 level,
						 Buffer *bufp)
{
	Page		page;
	LionPageOpaque opaque;

	lion_verify_visit(vs, blk, "a posting tree");
	page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, bufp);
	opaque = LionPageGetOpaque(page);

	/*
	 * DESIGN.md §18.  A live entry must not reach a freed page at all, and
	 * every page of a posting set has to name that set: readers rely on both
	 * to tell a set they still hold a link to from one whose pages have been
	 * handed to somebody else.
	 */
	if (LionPageIsDeleted(page))
		lion_corrupt("lion index \"%s\": block %u is reachable from chain entry %u on block %u but is marked deleted",
					RelationGetRelationName(vs->index), blk, eoff, eblk);

	if (opaque->owner_head != entry->head ||
		opaque->owner_hash != entry->hash)
		lion_corrupt("lion index \"%s\": block %u of chain entry %u on block %u is owned by hash %u at head %u, expected hash %u at head %u",
					RelationGetRelationName(vs->index), blk, eoff, eblk,
					opaque->owner_hash, opaque->owner_head,
					entry->hash, entry->head);

	if (opaque->level != level)
		lion_corrupt("lion index \"%s\": posting page %u of chain entry %u on block %u is at level %u, expected %u",
					RelationGetRelationName(vs->index), blk, eoff, eblk,
					opaque->level, level);

	if (LionPageIncompleteSplit(page))
		ereport(WARNING,
				(errmsg("lion index \"%s\": posting page %u has an unfinished split",
						RelationGetRelationName(vs->index), blk),
				 errdetail("Its right sibling has no downlink in the parent yet."),
				 errhint("The next INSERT into that key repairs it.")));

	return page;
}

/*
 * Walk the LEAF chain of a posting set, left to right: the items of each page
 * and the ascending ckey order within and across pages.
 */
static void
lion_verify_posting_leaves(LionVerifyState *vs, BlockNumber eblk,
						   OffsetNumber eoff, const LionEntryTuple *entry,
						   BlockNumber first, LionVerifyPLevel *out,
						   uint64 *cardp, uint32 *ncontainersp)
{
	BlockNumber blk = first;
	BlockNumber last = InvalidBlockNumber;
	bool		haveprev = false;
	uint32		prevckey = 0;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionPageOpaque opaque;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber firstused = InvalidOffsetNumber;
		OffsetNumber lastused = InvalidOffsetNumber;
		uint32		minckey = 0;
		uint32		maxckey = 0;

		page = lion_verify_posting_page(vs, blk, eblk, eoff, entry, 0, &buf);
		opaque = LionPageGetOpaque(page);
		maxoff = PageGetMaxOffsetNumber(page);

		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = lion_verify_itemid(vs, blk, page, off);
			LionContainer *c;

			if (!ItemIdIsUsed(iid))
				continue;

			c = (LionContainer *) PageGetItem(page, iid);
			lion_verify_container(vs, blk, off, c, ItemIdGetLength(iid),
								 &haveprev, &prevckey);

			*cardp += c->cardinality;
			(*ncontainersp)++;

			if (firstused == InvalidOffsetNumber)
				firstused = off;
			lastused = off;
		}

		/* min/max in the special area must describe the items */
		if (firstused == InvalidOffsetNumber)
		{
			if (opaque->minckey != 0 || opaque->maxckey != 0)
				lion_corrupt("lion index \"%s\": empty container page %u has minckey %u and maxckey %u, expected 0 and 0",
							RelationGetRelationName(vs->index), blk,
							opaque->minckey, opaque->maxckey);
		}
		else
		{
			minckey =
				lion_item_first_ckey((LionContainer *)
									PageGetItem(page, PageGetItemId(page, firstused)));
			maxckey =
				lion_item_last_ckey((LionContainer *)
								   PageGetItem(page, PageGetItemId(page, lastused)));

			if (opaque->minckey != minckey || opaque->maxckey != maxckey)
				lion_corrupt("lion index \"%s\": container page %u has minckey %u and maxckey %u, but holds %u .. %u",
							RelationGetRelationName(vs->index), blk,
							opaque->minckey, opaque->maxckey, minckey, maxckey);
		}

		lion_verify_plevel_add(out, blk, minckey,
							   firstused != InvalidOffsetNumber, maxckey, 0,
							   false, LionPageIncompleteSplit(page));

		last = blk;
		blk = opaque->rightlink;
		UnlockReleaseBuffer(buf);

		CHECK_FOR_INTERRUPTS();
	}

	if (last != entry->tail)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u ends at block %u, but its tail is block %u",
					RelationGetRelationName(vs->index), eoff, eblk, last,
					entry->tail);
}

/* Walk one INTERNAL level of a posting set, collecting its downlinks. */
static void
lion_verify_posting_level(LionVerifyState *vs, BlockNumber eblk,
						  OffsetNumber eoff, const LionEntryTuple *entry,
						  BlockNumber first, uint16 level,
						  LionVerifyPLevel *out, LionVerifyPLevel *children)
{
	BlockNumber blk = first;

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber firstdata;
		uint32		highkey = 0;
		bool		hashigh = false;
		uint32		firstkey = 0;
		bool		hasfirst = false;
		uint32		prevkey = 0;

		page = lion_verify_posting_page(vs, blk, eblk, eoff, entry, level, &buf);
		maxoff = PageGetMaxOffsetNumber(page);
		firstdata = lion_posting_first_data(page);

		/* Every item is one whole pivot before any of them is read. */
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = lion_verify_itemid(vs, blk, page, off);

			if (!ItemIdIsUsed(iid) ||
				ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE)
				lion_corrupt("lion index \"%s\": item %u of internal posting page %u is %zu bytes, expected %zu",
							RelationGetRelationName(vs->index), off, blk,
							(Size) ItemIdGetLength(iid),
							LION_POSTING_PIVOT_SIZE);
		}

		if (!LionPageIsRightmost(page))
		{
			if (maxoff < FirstOffsetNumber)
				lion_corrupt("lion index \"%s\": internal posting page %u is not rightmost but has no high key",
							RelationGetRelationName(vs->index), blk);
			if (BlockNumberIsValid(lion_posting_pivot(page, FirstOffsetNumber)->child))
				lion_corrupt("lion index \"%s\": the first item of internal posting page %u is a downlink, not a high key",
							RelationGetRelationName(vs->index), blk);
			highkey = lion_posting_pivot(page, FirstOffsetNumber)->ckey;
			hashigh = true;
		}
		else if (maxoff >= FirstOffsetNumber &&
				 !BlockNumberIsValid(lion_posting_pivot(page, FirstOffsetNumber)->child))
			lion_corrupt("lion index \"%s\": rightmost internal posting page %u carries a high key",
						RelationGetRelationName(vs->index), blk);

		if (firstdata > maxoff)
			lion_corrupt("lion index \"%s\": internal posting page %u has no downlink",
						RelationGetRelationName(vs->index), blk);

		for (off = firstdata; off <= maxoff; off++)
		{
			LionPostingPivot *piv = lion_posting_pivot(page, off);

			if (!BlockNumberIsValid(piv->child))
				lion_corrupt("lion index \"%s\": item %u of internal posting page %u is a second high key",
							RelationGetRelationName(vs->index), off, blk);

			/*
			 * Separators are non-decreasing rather than strictly increasing: a
			 * split whose two halves are both empty by the time its repair runs
			 * gives the right one the left one's separator, which is a range of
			 * zero keys and routes everything to the right page (DESIGN.md §22).
			 */
			if (hasfirst && piv->ckey < prevkey)
				lion_corrupt("lion index \"%s\": downlink %u of internal posting page %u has separator %u, below the one before it (%u)",
							RelationGetRelationName(vs->index), off, blk,
							piv->ckey, prevkey);
			if (hashigh && piv->ckey >= highkey)
				lion_corrupt("lion index \"%s\": downlink %u of internal posting page %u has separator %u, not below its high key %u",
							RelationGetRelationName(vs->index), off, blk,
							piv->ckey, highkey);

			if (!hasfirst)
			{
				firstkey = piv->ckey;
				hasfirst = true;
			}
			prevkey = piv->ckey;

			if (children != NULL)
				lion_verify_plevel_add(children, piv->child, piv->ckey, true,
									   piv->ckey, 0, false, false);

			CHECK_FOR_INTERRUPTS();
		}

		lion_verify_plevel_add(out, blk, firstkey, hasfirst, prevkey, highkey,
							   hashigh, LionPageIncompleteSplit(page));

		blk = LionPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buf);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Walk and check the posting tree of a CHAIN entry (DESIGN.md §22).
 *
 * The tree is checked level by level from the leaves up, exactly as §21's
 * directory is: each level is walked along its right links, which proves the
 * sibling chain and the key order within and across its pages, and the level
 * above is then checked against what that walk collected, which proves that
 * its downlinks name exactly those pages in that order - which is the same
 * statement as "the leaf right-link chain equals the in-order leaf sequence".
 */
static void
lion_verify_chain(LionVerifyState *vs, BlockNumber eblk, OffsetNumber eoff,
				 const LionEntryTuple *entry)
{
	BlockNumber leftmost[LION_POSTING_MAX_HEIGHT + 1];
	LionVerifyPLevel *lvl;
	uint64		card = 0;
	uint32		ncontainers = 0;
	uint32		height = 0;
	uint32		i;
	int			j;

	if (!BlockNumberIsValid(entry->head) || !BlockNumberIsValid(entry->tail))
		lion_corrupt("lion index \"%s\": chain entry %u on block %u has head %u and tail %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->head, entry->tail);

	/*
	 * The leftmost page of every level, from the root down.  The pages are
	 * read, not visited: the level walks below visit them.  An ERROR raised
	 * with a buffer locked releases it on the way out, as everywhere else in
	 * this file.
	 */
	{
		BlockNumber blk = entry->head;
		int			steps = 0;
		uint16		expect = 0;

		for (i = 0; i <= LION_POSTING_MAX_HEIGHT; i++)
			leftmost[i] = InvalidBlockNumber;

		for (;;)
		{
			Buffer		buf;
			Page		page;
			uint16		level;
			OffsetNumber firstdata;
			ItemId		iid;

			if (steps++ > LION_POSTING_MAX_HEIGHT)
				lion_corrupt("lion index \"%s\": the posting tree of chain entry %u on block %u is deeper than %d levels",
							RelationGetRelationName(vs->index), eoff, eblk,
							LION_POSTING_MAX_HEIGHT);

			/* the block's range, the header and the page kind */
			page = lion_verify_read_page(vs, blk, LION_PAGE_CONTAINER, &buf);

			level = LionPageGetOpaque(page)->level;
			if (level > LION_POSTING_MAX_HEIGHT)
				lion_corrupt("lion index \"%s\": posting page %u claims level %u",
							RelationGetRelationName(vs->index), blk, level);
			if (blk == entry->head)
				height = level;
			else if (level != expect)
				lion_corrupt("lion index \"%s\": posting page %u is at level %u, but its parent is at level %u",
							RelationGetRelationName(vs->index), blk, level,
							expect + 1);
			if (!LionPageIsRightmost(page) && blk == entry->head)
				lion_corrupt("lion index \"%s\": the root %u of chain entry %u on block %u has a right sibling",
							RelationGetRelationName(vs->index), blk, eoff, eblk);
			leftmost[level] = blk;
			if (level == 0)
			{
				UnlockReleaseBuffer(buf);
				break;
			}

			firstdata = lion_posting_first_data(page);
			if (firstdata > PageGetMaxOffsetNumber(page))
				lion_corrupt("lion index \"%s\": internal posting page %u has no downlink",
							RelationGetRelationName(vs->index), blk);
			iid = lion_verify_itemid(vs, blk, page, firstdata);
			if (!ItemIdIsUsed(iid) ||
				ItemIdGetLength(iid) != LION_POSTING_PIVOT_SIZE)
				lion_corrupt("lion index \"%s\": item %u of internal posting page %u is %zu bytes, expected %zu",
							RelationGetRelationName(vs->index), firstdata, blk,
							(Size) ItemIdGetLength(iid),
							LION_POSTING_PIVOT_SIZE);

			/* checked against the index's length when it is read */
			blk = lion_posting_pivot(page, firstdata)->child;
			expect = level - 1;
			UnlockReleaseBuffer(buf);
		}
	}

	lvl = (LionVerifyPLevel *) palloc0(sizeof(LionVerifyPLevel) * (height + 1));

	lion_verify_posting_leaves(vs, eblk, eoff, entry, leftmost[0], &lvl[0],
							   &card, &ncontainers);

	for (i = 1; i <= height; i++)
	{
		LionVerifyPLevel children;
		const LionVerifyPLevel *below = &lvl[i - 1];
		int			p;

		memset(&children, 0, sizeof(children));
		lion_verify_posting_level(vs, eblk, eoff, entry, leftmost[i],
								  (uint16) i, &lvl[i], &children);

		/*
		 * Match the downlinks, in the order the walk of this level found
		 * them, with the pages of the level below, in the order ITS walk
		 * found them.  j is the next downlink to match.
		 *
		 * One page may lack a downlink without being damage: the right
		 * sibling a split made, while the split is unfinished.  The split
		 * writes the sibling in one record and its downlink in the next, and
		 * leaves the LEFT page flagged LION_PAGE_INCOMPLETE_SPLIT in between;
		 * a crash - or an error - there leaves it so until the next writer
		 * that descends to the left page finishes it (DESIGN.md §22).  The
		 * sibling is then reachable through its left neighbour's right link
		 * and from nothing above, and the walk has already warned about the
		 * flag.  A three-way split flags both of its first two pages, so each
		 * missing downlink is covered by the page to its own left.  Such a
		 * page lies in its left neighbour's key range until the split
		 * finishes, and is bounded above by the next downlink's separator
		 * like any other page.
		 */
		j = 0;
		for (p = 0; p < below->npages; p++)
		{
			if (j < children.npages && children.blocks[j] == below->blocks[p])
			{
				uint32		sep = children.firstkey[j];

				if (j == 0 && sep != 0)
					lion_corrupt("lion index \"%s\": the first downlink of posting level %u has separator %u, expected minus infinity",
								RelationGetRelationName(vs->index), i, sep);

				/* the separator is at or below its child's own first key */
				if (below->hasfirst[p] && sep > below->firstkey[p])
					lion_corrupt("lion index \"%s\": the separator %u of posting block %u sorts after its own first container key %u",
								RelationGetRelationName(vs->index), sep,
								below->blocks[p], below->firstkey[p]);
				j++;
			}
			else if (p > 0 && below->incomplete[p - 1])
			{
				/* the right half of an unfinished split: no downlink yet */
			}
			else if (j < children.npages)
				lion_corrupt("lion index \"%s\": downlink %d of posting level %u points at block %u, but the next page of level %u is block %u",
							RelationGetRelationName(vs->index), j, i,
							children.blocks[j], i - 1, below->blocks[p]);
			else
				lion_corrupt("lion index \"%s\": posting level %u of chain entry %u on block %u has %d downlinks but level %u has %d pages",
							RelationGetRelationName(vs->index), i, eoff, eblk,
							children.npages, i - 1, below->npages);

			/* ... and the page's own upper bound is below the next one */
			if (j < children.npages)
			{
				uint32		next = children.firstkey[j];

				if (i == 1)
				{
					if (below->hasfirst[p] && below->lastkey[p] >= next)
						lion_corrupt("lion index \"%s\": leaf %u holds container key %u, at or above the separator %u of its right sibling",
									RelationGetRelationName(vs->index),
									below->blocks[p], below->lastkey[p], next);
				}
				else if (below->hashigh[p] && below->highkey[p] > next)
					lion_corrupt("lion index \"%s\": the high key %u of posting block %u sorts after the separator %u of its right sibling",
								RelationGetRelationName(vs->index),
								below->highkey[p], below->blocks[p], next);
			}
		}

		if (j < children.npages)
			lion_corrupt("lion index \"%s\": posting level %u of chain entry %u on block %u has %d downlinks but level %u has %d pages",
						RelationGetRelationName(vs->index), i, eoff, eblk,
						children.npages, i - 1, below->npages);
	}

	if (height > vs->max_posting_height)
		vs->max_posting_height = height;

	if (card != entry->ntids)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u claims " UINT64_FORMAT " TIDs, but its containers hold " UINT64_FORMAT,
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->ntids, card);

	if (ncontainers != entry->ncontainers)
		lion_corrupt("lion index \"%s\": chain entry %u on block %u claims %u containers, but its posting tree holds %u",
					RelationGetRelationName(vs->index), eoff, eblk,
					entry->ncontainers, ncontainers);
}

/*
 * Check one entry tuple.
 *
 * The walk has put every item of the page through lion_verify_dir_item()
 * already, but this does not lean on it for the order of its own reads: the
 * size is checked before any header field is read, and the key's extent and
 * length before the key is hashed.
 */
static void
lion_verify_entry(LionVerifyState *vs, BlockNumber blk,
				 OffsetNumber off, ItemId iid, Page page)
{
	LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);
	Size		itemsz = ItemIdGetLength(iid);
	uint16		kind;
	LionState  *state;

	if (itemsz < LION_ENTRY_HDRSZ)
		lion_corrupt("lion index \"%s\": entry %u on block %u is only %zu bytes",
					RelationGetRelationName(vs->index), off, blk, itemsz);

	if (itemsz < LionEntryPayloadOffset(entry))
		lion_corrupt("lion index \"%s\": entry %u on block %u is %zu bytes, too small for its %u byte key",
					RelationGetRelationName(vs->index), off, blk, itemsz,
					entry->keylen);

	kind = entry->flags & (LION_ENTRY_INLINE | LION_ENTRY_CHAIN);

	/*
	 * The KEY COLUMN (DESIGN.md §24).  Everything below - the key length, the
	 * hash, whether an EMPTY entry may exist at all - is decided by that
	 * column's opclass, and the columns' entry runs must come out in attno
	 * order, which the directory comparator enforces item by item and this
	 * checks again across pages.
	 */
	if (entry->attno < 1 || entry->attno > vs->ix->ncolumns)
		lion_corrupt("lion index \"%s\": entry %u on block %u belongs to key column %u, but the index has %d",
					RelationGetRelationName(vs->index), off, blk, entry->attno,
					vs->ix->ncolumns);
	if (entry->attno < vs->lastattno)
		lion_corrupt("lion index \"%s\": entry %u on block %u belongs to key column %u, below the column %u of the entry before it",
					RelationGetRelationName(vs->index), off, blk, entry->attno,
					vs->lastattno);
	vs->lastattno = entry->attno;
	state = lion_column(vs->ix, (AttrNumber) entry->attno);

	if (entry->unused != 0)
		lion_corrupt("lion index \"%s\": entry %u on block %u has a non-zero reserved header field",
					RelationGetRelationName(vs->index), off, blk);

	if (kind != LION_ENTRY_INLINE && kind != LION_ENTRY_CHAIN)
		lion_corrupt("lion index \"%s\": entry %u on block %u has flags 0x%04X, expected exactly one of INLINE and CHAIN",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	if ((entry->flags & ~(uint16) (LION_ENTRY_INLINE | LION_ENTRY_CHAIN |
								   LION_ENTRY_RESERVED)) != 0)
		lion_corrupt("lion index \"%s\": entry %u on block %u has unknown flag bits in 0x%04X",
					RelationGetRelationName(vs->index), off, blk, entry->flags);

	if ((entry->flags & LION_ENTRY_RESERVED) == LION_ENTRY_RESERVED)
		lion_corrupt("lion index \"%s\": entry %u on block %u is both the null and the empty entry",
					RelationGetRelationName(vs->index), off, blk);

	if (LionEntryIsReserved(entry))
	{
		/*
		 * A reserved entry (DESIGN.md §14 and §17): no key bytes, hash 0,
		 * bucket 0, and one of each per index at most - a second one would
		 * split its rows between two entries that no reader looks for twice.
		 */
		const char *what = LionEntryIsNullKey(entry) ? "null" : "empty";

		if (entry->keylen != 0)
			lion_corrupt("lion index \"%s\": %s entry %u on block %u has a key of %u bytes",
						RelationGetRelationName(vs->index), what, off, blk,
						entry->keylen);
		if (entry->hash != LION_NULLKEY_HASH)
			lion_corrupt("lion index \"%s\": %s entry %u on block %u stores hash %u, expected %d",
						RelationGetRelationName(vs->index), what, off, blk,
						entry->hash, LION_NULLKEY_HASH);
		if (LionEntryIsNullKey(entry) ?
			(++vs->nnullentries[entry->attno - 1] > 1) :
			(++vs->nemptyentries[entry->attno - 1] > 1))
			lion_corrupt("lion index \"%s\": entry %u on block %u is a second %s entry for key column %u",
						RelationGetRelationName(vs->index), off, blk, what,
						entry->attno);

		/*
		 * Only a multi-key opclass ever writes an empty entry; finding one in
		 * a scalar index means the two flag bits have been confused
		 * somewhere.
		 */
		if (LionEntryIsEmptyKey(entry) && !state->multikey)
			lion_corrupt("lion index \"%s\": entry %u on block %u is an empty-key entry, but the operator class of key column %u extracts no keys",
						RelationGetRelationName(vs->index), off, blk,
						entry->attno);
	}
	else
	{
		uint32		hash;

		lion_verify_keylen(vs, state, blk, off, entry);

		hash = lion_hash_key(state,
							lion_fetch_key(state, LionEntryGetKey(entry)));
		if (hash != entry->hash)
			lion_corrupt("lion index \"%s\": entry %u on block %u stores hash %u, but its key hashes to %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->hash, hash);
	}

	if (kind == LION_ENTRY_INLINE)
	{
		Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
		Size		cur = 0;
		Size		csize;
		bool		haveprev = false;
		uint32		prevckey = 0;
		uint64		card = 0;
		uint32		ncontainers = 0;

		if (BlockNumberIsValid(entry->head) || BlockNumberIsValid(entry->tail))
			lion_corrupt("lion index \"%s\": inline entry %u on block %u has head %u and tail %u, expected none",
						RelationGetRelationName(vs->index), off, blk,
						entry->head, entry->tail);

		while ((csize = lion_inline_fetch(LionEntryGetPayload(entry), paylen,
										 &cur, vs->cbuf)) > 0)
		{
			lion_verify_container(vs, blk, off, vs->cbuf, csize,
								 &haveprev, &prevckey);
			card += vs->cbuf->cardinality;
			ncontainers++;
		}

		/*
		 * Whatever is left is the payload's growth slack: the zeroed tail an
		 * INSERT leaves so that the next member fits without rewriting the
		 * entry (DESIGN.md §4), or the one VACUUM leaves when it writes a
		 * shrunken payload back into the bytes the entry already had (§18).
		 * Both are bounded by the same constant, so that slack cannot hide a
		 * malformed payload, and every byte of it has to really be zero -
		 * which is also what makes it a terminator.
		 */
		if (paylen - cur > LION_ENTRY_SLACK_BOUND)
			lion_corrupt("lion index \"%s\": entry %u on block %u has %zu bytes of payload slack, at most %d allowed",
						RelationGetRelationName(vs->index), off, blk,
						paylen - cur, LION_ENTRY_SLACK_BOUND);
		{
			const char *pay = LionEntryGetPayload(entry);
			Size		i;

			for (i = cur; i < paylen; i++)
			{
				if (pay[i] != 0)
					lion_corrupt("lion index \"%s\": entry %u on block %u has a non-zero byte at payload offset %zu, past its last item",
								RelationGetRelationName(vs->index), off, blk, i);
			}
		}

		if (card != entry->ntids)
			lion_corrupt("lion index \"%s\": inline entry %u on block %u claims " UINT64_FORMAT " TIDs, but its payload holds " UINT64_FORMAT,
						RelationGetRelationName(vs->index), off, blk,
						entry->ntids, card);

		if (ncontainers != entry->ncontainers)
			lion_corrupt("lion index \"%s\": inline entry %u on block %u claims %u containers, but its payload holds %u",
						RelationGetRelationName(vs->index), off, blk,
						entry->ncontainers, ncontainers);
	}
	else
	{
		if (itemsz != LionEntryPayloadOffset(entry))
			lion_corrupt("lion index \"%s\": chain entry %u on block %u is %zu bytes, expected %zu",
						RelationGetRelationName(vs->index), off, blk, itemsz,
						LionEntryPayloadOffset(entry));

		lion_verify_chain(vs, blk, off, entry);
	}
}

/* ---------------------------------------------------------------------
 * The directory (DESIGN.md §21)
 *
 * The tree is checked level by level from the leaves up.  Each level is
 * walked along its right links, which proves the sibling links and the key
 * order within and across its pages; the level above is then checked against
 * what the walk of the level below collected, which proves that its downlinks
 * name exactly those pages, in that order, with separators that bound them.
 * Every page is passed through lion_verify_visit(), so a page reachable twice
 * - or not at all - is reported by that and by lion_verify_reachable().
 * --------------------------------------------------------------------- */

typedef struct LionVerifyLevel
{
	int			npages;
	int			maxpages;
	BlockNumber *blocks;
	LionEntryTuple **firstkey;	/* first data item of each page, or NULL */
	LionEntryTuple **highkey;	/* its high key, or NULL when rightmost */
	bool	   *incomplete;		/* flagged LION_PAGE_INCOMPLETE_SPLIT */
} LionVerifyLevel;

static void
lion_verify_level_add(LionVerifyLevel *lvl, BlockNumber blk,
					 LionEntryTuple *firstkey, LionEntryTuple *highkey,
					 bool incomplete)
{
	if (lvl->npages >= lvl->maxpages)
	{
		bool		first = (lvl->maxpages == 0);

		lvl->maxpages = first ? 64 : lvl->maxpages * 2;
		if (first)
		{
			lvl->blocks = (BlockNumber *) palloc(sizeof(BlockNumber) * lvl->maxpages);
			lvl->firstkey = (LionEntryTuple **) palloc(sizeof(LionEntryTuple *) * lvl->maxpages);
			lvl->highkey = (LionEntryTuple **) palloc(sizeof(LionEntryTuple *) * lvl->maxpages);
			lvl->incomplete = (bool *) palloc(sizeof(bool) * lvl->maxpages);
		}
		else
		{
			lvl->blocks = (BlockNumber *) repalloc(lvl->blocks,
												   sizeof(BlockNumber) * lvl->maxpages);
			lvl->firstkey = (LionEntryTuple **) repalloc(lvl->firstkey,
														 sizeof(LionEntryTuple *) * lvl->maxpages);
			lvl->highkey = (LionEntryTuple **) repalloc(lvl->highkey,
														sizeof(LionEntryTuple *) * lvl->maxpages);
			lvl->incomplete = (bool *) repalloc(lvl->incomplete,
												sizeof(bool) * lvl->maxpages);
		}
	}
	lvl->blocks[lvl->npages] = blk;
	lvl->firstkey[lvl->npages] = firstkey;
	lvl->highkey[lvl->npages] = highkey;
	lvl->incomplete[lvl->npages] = incomplete;
	lvl->npages++;
}

static LionEntryTuple *
lion_verify_copy_item(Page page, OffsetNumber off)
{
	Size		sz = ItemIdGetLength(PageGetItemId(page, off));
	LionEntryTuple *c = (LionEntryTuple *) palloc(sz);

	memcpy(c, PageGetItem(page, PageGetItemId(page, off)), sz);
	return c;
}

/*
 * One equality class, one entry (DESIGN.md §21).  The entries whose prefixes
 * tie - (column, kind, proc 4, hash) - are the candidates for being the same
 * key, and they are contiguous in the leaf order, across page boundaries too,
 * so the check keeps the current prefix run and compares each new entry with
 * every member of it under the opclass equality.  Byte-identical twins are
 * caught by the ordering checks already; this also catches two spellings of
 * one class (the citext shape), which sort apart inside their run.  Runs are
 * a single entry unless the hash collides, so this is linear in practice.
 */
typedef struct LionVerifyRun
{
	int			n;
	int			max;
	LionEntryTuple **items;		/* copies */
	BlockNumber *blocks;
} LionVerifyRun;

static void
lion_verify_run_add(LionVerifyState *vs, LionVerifyRun *run,
					LionEntryTuple *item, BlockNumber blk, OffsetNumber off)
{
	int			i;

	if (run->n > 0)
	{
		LionSearchKey sk;

		lion_search_key_exact(vs->ix, &sk, run->items[0]);
		if (lion_cmp_prefix(item, &sk) != 0)
		{
			for (i = 0; i < run->n; i++)
				pfree(run->items[i]);
			run->n = 0;
		}
	}

	for (i = 0; i < run->n; i++)
	{
		LionEntryTuple *other = run->items[i];
		bool		same;

		if (lion_entry_kind(item) != LION_KIND_VALUE)
			same = true;		/* a second reserved entry of its kind */
		else
		{
			LionState  *col = lion_column(vs->ix, (AttrNumber) item->attno);

			same = lion_keys_equal(col,
								   lion_fetch_key(col, LionEntryGetKey(other)),
								   lion_fetch_key(col, LionEntryGetKey(item)));
		}
		if (same)
			lion_corrupt("lion index \"%s\": entry %u on block %u is a second entry for the key of an entry on block %u",
						RelationGetRelationName(vs->index), off, blk,
						run->blocks[i]);
	}

	if (run->n >= run->max)
	{
		run->max = (run->max == 0) ? 8 : run->max * 2;
		if (run->items == NULL)
		{
			run->items = (LionEntryTuple **) palloc(sizeof(LionEntryTuple *) * run->max);
			run->blocks = (BlockNumber *) palloc(sizeof(BlockNumber) * run->max);
		}
		else
		{
			run->items = (LionEntryTuple **) repalloc(run->items,
													  sizeof(LionEntryTuple *) * run->max);
			run->blocks = (BlockNumber *) repalloc(run->blocks,
												   sizeof(BlockNumber) * run->max);
		}
	}
	{
		Size		sz = MAXALIGN(LION_ENTRY_HDRSZ + item->keylen);
		LionEntryTuple *c = (LionEntryTuple *) palloc(sz);

		memcpy(c, item, LION_ENTRY_HDRSZ + item->keylen);
		run->items[run->n] = c;
		run->blocks[run->n] = blk;
		run->n++;
	}
}

/*
 * Walk one level of the directory along its right links.  For an internal
 * level the downlinks and their separators are collected into *children.
 */
static void
lion_verify_walk_level(LionVerifyState *vs, BlockNumber first, uint16 level,
					  bool isleaf, LionVerifyLevel *out,
					  LionVerifyLevel *children)
{
	BlockNumber blk = first;
	BlockNumber prev = InvalidBlockNumber;
	LionEntryTuple *prevhigh = NULL;
	LionVerifyRun run;

	memset(&run, 0, sizeof(run));

	while (BlockNumberIsValid(blk))
	{
		Buffer		buf;
		Page		page;
		LionPageOpaque opaque;
		OffsetNumber maxoff;
		OffsetNumber off;
		OffsetNumber firstdata;
		LionEntryTuple *prevkey = NULL;
		LionEntryTuple *firstkey = NULL;
		LionEntryTuple *highkey = NULL;

		lion_verify_visit(vs, blk, "the directory");
		page = lion_verify_read_page(vs, blk,
									 isleaf ? LION_PAGE_BUCKET : LION_PAGE_DIR,
									 &buf);
		opaque = LionPageGetOpaque(page);

		if (opaque->level != level)
			lion_corrupt("lion index \"%s\": directory page %u is at level %u, expected %u",
						RelationGetRelationName(vs->index), blk, opaque->level,
						level);
		if (opaque->leftlink != prev)
			lion_corrupt("lion index \"%s\": directory page %u has left link %u, expected %u",
						RelationGetRelationName(vs->index), blk, opaque->leftlink,
						prev);
		if (LionPageIsRoot(page) != (level == vs->height))
			lion_corrupt("lion index \"%s\": directory page %u at level %u %s the root flag",
						RelationGetRelationName(vs->index), blk, level,
						LionPageIsRoot(page) ? "should not have" : "should have");

		if (LionPageIncompleteSplit(page))
			ereport(WARNING,
					(errmsg("lion index \"%s\": directory page %u has an unfinished split",
							RelationGetRelationName(vs->index), blk),
					 errdetail("Its right sibling has no downlink in the parent yet."),
					 errhint("The next INSERT that descends to this page repairs it.")));

		maxoff = PageGetMaxOffsetNumber(page);
		firstdata = lion_page_first_data(page);

		/* Nothing below reads an item this has not vouched for. */
		for (off = FirstOffsetNumber; off <= maxoff; off++)
			(void) lion_verify_dir_item(vs, blk, page, off);

		if (!LionPageIsRightmost(page))
		{
			if (maxoff < FirstOffsetNumber)
				lion_corrupt("lion index \"%s\": directory page %u is not rightmost but has no high key",
							RelationGetRelationName(vs->index), blk);
			if (!ItemIdIsUsed(PageGetItemId(page, FirstOffsetNumber)) ||
				!LionEntryIsHighKey(lion_page_entry(page, FirstOffsetNumber)))
				lion_corrupt("lion index \"%s\": the first item of directory page %u is not a high key",
							RelationGetRelationName(vs->index), blk);
			highkey = lion_verify_copy_item(page, FirstOffsetNumber);
		}
		else if (maxoff >= FirstOffsetNumber &&
				 ItemIdIsUsed(PageGetItemId(page, FirstOffsetNumber)) &&
				 LionEntryIsHighKey(lion_page_entry(page, FirstOffsetNumber)))
			lion_corrupt("lion index \"%s\": rightmost directory page %u carries a high key",
						RelationGetRelationName(vs->index), blk);

		for (off = firstdata; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			LionEntryTuple *item;

			if (!ItemIdIsUsed(iid))
				continue;
			item = (LionEntryTuple *) PageGetItem(page, iid);

			if (LionEntryIsHighKey(item))
				lion_corrupt("lion index \"%s\": item %u of directory page %u is a second high key",
							RelationGetRelationName(vs->index), off, blk);
			if (isleaf == LionEntryIsDownlink(item))
				lion_corrupt("lion index \"%s\": item %u of directory page %u is %sa downlink",
							RelationGetRelationName(vs->index), off, blk,
							isleaf ? "" : "not ");

			/* Strictly increasing within the page (DESIGN.md §21). */
			if (prevkey != NULL &&
				lion_cmp_entries(vs->ix, prevkey, item) >= 0)
				lion_corrupt("lion index \"%s\": item %u of directory page %u does not sort after the one before it",
							RelationGetRelationName(vs->index), off, blk);

			if (firstkey == NULL)
			{
				firstkey = lion_verify_copy_item(page, off);
				if (prevhigh != NULL &&
					lion_cmp_entries(vs->ix, prevhigh, firstkey) > 0)
					lion_corrupt("lion index \"%s\": the high key of the page left of %u sorts after its first key",
								RelationGetRelationName(vs->index), blk);
			}
			if (prevkey != NULL)
				pfree(prevkey);
			prevkey = lion_verify_copy_item(page, off);

			if (isleaf)
			{
				lion_verify_entry(vs, blk, off, iid, page);
				lion_verify_run_add(vs, &run, item, blk, off);
			}
			else
			{
				if (children != NULL)
					lion_verify_level_add(children, item->head,
										  lion_verify_copy_item(page, off),
										  NULL, false);
			}

			CHECK_FOR_INTERRUPTS();
		}

		if (highkey != NULL && prevkey != NULL &&
			lion_cmp_entries(vs->ix, prevkey, highkey) >= 0)
			lion_corrupt("lion index \"%s\": the last key of directory page %u is not below its high key",
						RelationGetRelationName(vs->index), blk);
		if (prevkey != NULL)
			pfree(prevkey);

		lion_verify_level_add(out, blk, firstkey, highkey,
							  LionPageIncompleteSplit(page));

		prev = blk;
		prevhigh = highkey;		/* owned by *out; not freed here */
		blk = opaque->rightlink;
		UnlockReleaseBuffer(buf);

		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Check the whole directory: every level, and every level against the one
 * below it.
 */
static void
lion_verify_directory(LionVerifyState *vs)
{
	BlockNumber *leftmost;
	LionVerifyLevel *lvl;
	uint32		h = vs->height;
	uint32		i;
	int			j;

	/* The leftmost page of every level, from the root down. */
	leftmost = (BlockNumber *) palloc(sizeof(BlockNumber) * (h + 1));
	{
		BlockNumber blk = vs->root;

		for (i = 0; i <= h; i++)
		{
			Buffer		buf;
			Page		page;
			uint16		level = (uint16) (h - i);

			page = lion_verify_read_page(vs, blk,
										 level == 0 ? LION_PAGE_BUCKET :
										 LION_PAGE_DIR, &buf);
			if (LionPageGetOpaque(page)->level != level)
				lion_corrupt("lion index \"%s\": the leftmost page %u is at level %u, expected %u",
							RelationGetRelationName(vs->index), blk,
							LionPageGetOpaque(page)->level, level);
			leftmost[level] = blk;
			if (level > 0)
			{
				LionEntryTuple *down = NULL;

				if (PageGetMaxOffsetNumber(page) >= lion_page_first_data(page))
					down = lion_verify_dir_item(vs, blk, page,
												lion_page_first_data(page));
				if (down == NULL)
					lion_corrupt("lion index \"%s\": internal page %u has no downlink",
								RelationGetRelationName(vs->index), blk);
				if (!LionEntryIsMinusInf(down))
					lion_corrupt("lion index \"%s\": the leftmost downlink of page %u is not minus infinity",
								RelationGetRelationName(vs->index), blk);
				/* checked against the index's length when it is read */
				blk = down->head;
			}
			UnlockReleaseBuffer(buf);
		}
	}

	lvl = (LionVerifyLevel *) palloc0(sizeof(LionVerifyLevel) * (h + 1));

	for (i = 0; i <= h; i++)
	{
		LionVerifyLevel children;

		memset(&children, 0, sizeof(children));
		lion_verify_walk_level(vs, leftmost[i], (uint16) i, i == 0, &lvl[i],
							   i == 0 ? NULL : &children);

		if (i > 0)
		{
			const LionVerifyLevel *below = &lvl[i - 1];
			int			p;

			/*
			 * Match the downlinks with the pages of the level below, each in
			 * the order its own walk found them; j is the next downlink to
			 * match.  A page may lack a downlink when its left neighbour is
			 * flagged LION_PAGE_INCOMPLETE_SPLIT: the right half of a split
			 * whose downlink record a crash or an error cut off, which the
			 * next writer that descends to the left page finishes (DESIGN.md
			 * §21) and which the walk has already warned about.  The flag
			 * with the downlink already in place is normal too - the flag is
			 * cleared by a record of its own - and needs nothing here.  The
			 * rules are the posting tree's, in lion_verify_chain().
			 */
			j = 0;
			for (p = 0; p < below->npages; p++)
			{
				if (j < children.npages && children.blocks[j] == below->blocks[p])
				{
					LionEntryTuple *sep = children.firstkey[j];

					if (j == 0)
					{
						if (!LionEntryIsMinusInf(sep))
							lion_corrupt("lion index \"%s\": the first downlink of level %u is not minus infinity",
										RelationGetRelationName(vs->index), i);
					}
					else if (LionEntryIsMinusInf(sep))
						lion_corrupt("lion index \"%s\": downlink %d of level %u is minus infinity",
									RelationGetRelationName(vs->index), j, i);
					else if (below->firstkey[p] != NULL &&
							 lion_cmp_entries(vs->ix, sep,
											  below->firstkey[p]) > 0)
						lion_corrupt("lion index \"%s\": the separator of block %u sorts after its own first key",
									RelationGetRelationName(vs->index),
									below->blocks[p]);
					j++;
				}
				else if (p > 0 && below->incomplete[p - 1])
				{
					/* the right half of an unfinished split: no downlink yet */
				}
				else if (j < children.npages)
					lion_corrupt("lion index \"%s\": downlink %d of level %u points at block %u, but the next page of level %u is block %u",
								RelationGetRelationName(vs->index), j, i,
								children.blocks[j], i - 1, below->blocks[p]);
				else
					lion_corrupt("lion index \"%s\": level %u has %d downlinks but level %u has %d pages",
								RelationGetRelationName(vs->index), i,
								children.npages, i - 1, below->npages);

				/*
				 * A child's high key was the next separator when the split
				 * made them; later splits of the child only lower it.
				 */
				if (j < children.npages &&
					below->highkey[p] != NULL &&
					lion_cmp_entries(vs->ix, below->highkey[p],
									 children.firstkey[j]) > 0)
					lion_corrupt("lion index \"%s\": the high key of block %u sorts after the separator of its right sibling",
								RelationGetRelationName(vs->index),
								below->blocks[p]);
			}

			if (j < children.npages)
				lion_corrupt("lion index \"%s\": level %u has %d downlinks but level %u has %d pages",
							RelationGetRelationName(vs->index), i,
							children.npages, i - 1, below->npages);
		}
	}

	pfree(leftmost);
}

/*
 * Check the meta page.
 */
static void
lion_verify_meta(LionVerifyState *vs)
{
	Buffer		buf;
	Page		page;
	LionMetaPageData *meta;

	lion_verify_visit(vs, LION_METAPAGE_BLKNO, "the meta page");
	page = lion_verify_read_page(vs, LION_METAPAGE_BLKNO, LION_PAGE_META, &buf);
	meta = LionPageGetMeta(page);

	if (meta->magic != LION_MAGIC || meta->version != LION_VERSION)
		lion_corrupt("lion index \"%s\": meta page has magic %08X version %u, expected %08X version %u",
					RelationGetRelationName(vs->index), meta->magic,
					meta->version, LION_MAGIC, LION_VERSION);

	if (meta->offset_bits != LION_OFFSET_BITS ||
		meta->container_bits != LION_CONTAINER_BITS)
		lion_corrupt("lion index \"%s\": meta page has offset_bits %u and container_bits %u, expected %d and %d",
					RelationGetRelationName(vs->index), meta->offset_bits,
					meta->container_bits, LION_OFFSET_BITS, LION_CONTAINER_BITS);

	if (meta->inline_limit < LION_MIN_INLINE_LIMIT ||
		meta->inline_limit > LION_MAX_INLINE_LIMIT)
		lion_corrupt("lion index \"%s\": meta page has inline_limit %u, expected %d .. %d",
					RelationGetRelationName(vs->index), meta->inline_limit,
					LION_MIN_INLINE_LIMIT, LION_MAX_INLINE_LIMIT);

	if (!BlockNumberIsValid(meta->root) || meta->root >= vs->nblocks)
		lion_corrupt("lion index \"%s\": meta page names root block %u, but the index has %u blocks",
					RelationGetRelationName(vs->index), meta->root, vs->nblocks);

	/*
	 * A directory of height h has at least h + 1 pages besides this one, and
	 * the walk sizes its arrays by the height: bound it before it is used.
	 */
	if ((uint64) meta->height + 2 > (uint64) vs->nblocks)
		lion_corrupt("lion index \"%s\": meta page has directory height %u, but the index has only %u blocks",
					RelationGetRelationName(vs->index), meta->height,
					vs->nblocks);

	if (LionPageGetOpaque(page)->rightlink != InvalidBlockNumber)
		lion_corrupt("lion index \"%s\": the meta page has a right link to block %u",
					RelationGetRelationName(vs->index),
					LionPageGetOpaque(page)->rightlink);

	vs->root = meta->root;
	vs->height = meta->height;

	UnlockReleaseBuffer(buf);
}

/*
 * Every block has to belong to the meta page, the directory or exactly one
 * key's container chain.
 *
 * Two kinds of unreferenced block are tolerated (with a warning), because a
 * crash or an error can leave them behind and neither one makes the index
 * wrong: a block that was never initialised (the relation was extended and
 * the transaction did not get as far as its WAL record) and an empty
 * container page (a multi-page spill that did not reach its entry update).
 */
static void
lion_verify_reachable(LionVerifyState *vs)
{
	BlockNumber blk;

	for (blk = 0; blk < vs->nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		bool		leaked;

		if (vs->refs[blk] != 0)
			continue;

		buf = ReadBuffer(vs->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		/*
		 * A DELETED page is the normal state of a freed one (DESIGN.md §18):
		 * it is unreferenced on purpose, it is in the free space map, and the
		 * next allocation whose safexid test it passes takes it.  Nothing to
		 * report.
		 */
		if (!PageIsNew(page) &&
			PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
			LionPageGetOpaque(page)->page_id == LION_PAGE_ID &&
			LionPageIsDeleted(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		/*
		 * An INTERNAL posting page is leaked as a whole page rather than as an
		 * empty one (DESIGN.md §22): a posting set is freed leaves-first, so a
		 * crash between deleting the entry and marking its pages free can
		 * leave the root of the set behind with its downlinks still on it.
		 * The next VACUUM's sweep recovers it once its children are gone.
		 */
		leaked = PageIsNew(page) ||
			(PageGetSpecialSize(page) == LION_SPECIAL_SIZE &&
			 LionPageGetOpaque(page)->page_id == LION_PAGE_ID &&
			 LionPageIsContainer(page) &&
			 (PageGetMaxOffsetNumber(page) == 0 ||
			  LionPageIsPostingInternal(page)));

		UnlockReleaseBuffer(buf);

		if (!leaked)
			lion_corrupt("lion index \"%s\": block %u is not reachable from the meta page",
						RelationGetRelationName(vs->index), blk);

		ereport(WARNING,
				(errmsg("lion index \"%s\": block %u is unused and unreachable",
						RelationGetRelationName(vs->index), blk),
				 errdetail("An interrupted page allocation leaks blocks; the next VACUUM turns them into free pages.")));
	}
}

/* ---------------------------------------------------------------------
 * heapallindexed
 * --------------------------------------------------------------------- */

/*
 * Is (ckey, lo) present in the posting set of key, or of the reserved entry
 * named by reservedflag when there is one (DESIGN.md §14 and §17)?
 */
static bool
lion_verify_tid_present(LionVerifyState *vs, LionState *state, Datum key,
					   uint16 reservedflag, uint32 hash, uint32 ckey,
					   uint16 lo)
{
	Relation	index = vs->index;
	Buffer		entrybuf;
	OffsetNumber entryoff;
	bool		present = false;

	if ((reservedflag != 0) ?
		lion_find_reserved_entry(index, state, BUFFER_LOCK_SHARE,
								reservedflag, &entrybuf, &entryoff) :
		lion_find_entry(index, state, BUFFER_LOCK_SHARE, key, hash,
					   &entrybuf, &entryoff))
	{
		Page		page = BufferGetPage(entrybuf);
		ItemId		iid = PageGetItemId(page, entryoff);
		LionEntryTuple *entry = (LionEntryTuple *) PageGetItem(page, iid);

		if ((entry->flags & LION_ENTRY_INLINE) != 0)
		{
			Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry, ItemIdGetLength(iid));
			Size		cur = 0;

			while (lion_inline_fetch(LionEntryGetPayload(entry), paylen, &cur,
									vs->cbuf) > 0)
			{
				if (lion_item_covers(vs->cbuf, ckey))
				{
					present = lion_item_contains(vs->cbuf, ckey, lo);
					break;
				}
				if (lion_item_first_ckey(vs->cbuf) > ckey)
					break;
			}
		}
		else
		{
			BlockNumber blk = lion_chain_find_page(index, entry->hash,
												  entry->head, entry->tail,
												  ckey);
			Buffer		cbuf;
			Page		cpage;
			OffsetNumber off;
			bool		found;

			cbuf = ReadBuffer(index, blk);
			LockBuffer(cbuf, BUFFER_LOCK_SHARE);
			cpage = BufferGetPage(cbuf);
			off = lion_page_find_item(cpage, ckey, &found);
			if (found)
				present = lion_item_contains((LionContainer *)
											PageGetItem(cpage,
														PageGetItemId(cpage, off)),
											ckey, lo);
			UnlockReleaseBuffer(cbuf);
		}

		UnlockReleaseBuffer(entrybuf);
	}
	else if (BufferIsValid(entrybuf))
		UnlockReleaseBuffer(entrybuf);

	return present;
}

/*
 * table_index_build_scan() callback: every heap tuple visible to our snapshot
 * has to be indexed under its key.
 */
static void
lion_verify_one_key(LionVerifyState *vs, LionState *state, ItemPointer tid,
				   Datum key, uint16 reservedflag, uint64 code)
{
	uint32		hash = (reservedflag != 0) ? LION_NULLKEY_HASH :
		lion_hash_key(state, key);
	bool		typisvarlena;

	if (lion_verify_tid_present(vs, state, key, reservedflag, hash,
							   lion_code_ckey(code), lion_code_lo(code)))
		return;

	/*
	 * The key's value goes into the message only for a superuser.  The
	 * function may be granted to roles without SELECT on the table, and it
	 * reads past column privileges and row-level security; the message would
	 * hand them the value, and put it in the server log.  The TID is enough
	 * to find the row.
	 *
	 * "A superuser" means the CALLER, which lion_index_verify() asked before
	 * it became the table owner: superuser() here would ask about the owner,
	 * and a table a superuser owns would then show its values to whoever the
	 * function was granted to.
	 */
	if (reservedflag == 0 && !vs->showvalues)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" is not indexed in \"%s\"",
						ItemPointerGetBlockNumber(tid),
						ItemPointerGetOffsetNumber(tid),
						RelationGetRelationName(vs->heap),
						RelationGetRelationName(vs->index)),
				 errdetail("Key column %u of the tuple is not NULL.", state->attno)));

	getTypeOutputInfo(state->typid, &vs->keyoutfunc, &typisvarlena);

	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("heap tuple (%u,%u) from table \"%s\" is not indexed in \"%s\"",
					ItemPointerGetBlockNumber(tid),
					ItemPointerGetOffsetNumber(tid),
					RelationGetRelationName(vs->heap),
					RelationGetRelationName(vs->index)),
			 errdetail("Key column %u of the tuple is %s.", state->attno,
					   (reservedflag == LION_ENTRY_NULLKEY) ? "NULL" :
					   (reservedflag == LION_ENTRY_EMPTYKEY) ? "absent" :
					   OidOutputFunctionCall(vs->keyoutfunc, key))));
}

/*
 * table_index_build_scan() callback: every heap tuple visible to our snapshot
 * has to be indexed under its key, in EVERY key column (DESIGN.md §24).
 */
static void
lion_verify_heap_callback(Relation index, ItemPointer tid, Datum *values,
						 bool *isnull, bool tupleIsAlive, void *arg)
{
	LionVerifyState *vs = (LionVerifyState *) arg;
	MemoryContext oldcxt;
	uint64		code;
	int			c;

	oldcxt = MemoryContextSwitchTo(vs->heapcxt);

	lion_check_key_offset(tid);
	code = lion_tid_to_code(tid);

	for (c = 0; c < vs->ix->ncolumns; c++)
	{
		LionState  *state = &vs->ix->cols[c];
		Datum		key;

		if (isnull[c])
		{
			/* NULL values live in the column's reserved entry (§14). */
			lion_verify_one_key(vs, state, tid, (Datum) 0,
							   LION_ENTRY_NULLKEY, code);
		}
		else if (state->multikey)
		{
			/*
			 * DESIGN.md §17: the row has to be present under EVERY key its
			 * value extracts to, and in the reserved EMPTY entry when it
			 * extracts to none.  Extracting here rather than trusting the
			 * index is the whole point of the check: it is the same call the
			 * build and the insert make, so a row that is missing under one
			 * of several keys is found.
			 */
			Datum	   *keys;
			int			nkeys = lion_extract_value(state, values[c], &keys);
			int			i;

			if (nkeys == 0)
				lion_verify_one_key(vs, state, tid, (Datum) 0,
								   LION_ENTRY_EMPTYKEY, code);
			for (i = 0; i < nkeys; i++)
				lion_verify_one_key(vs, state, tid, keys[i], 0, code);
		}
		else
		{
			key = values[c];
			if (!state->typbyval && state->typlen == -1)
				key = PointerGetDatum(PG_DETOAST_DATUM(key));

			lion_verify_one_key(vs, state, tid, key, 0, code);
		}

		CHECK_FOR_INTERRUPTS();
	}

	vs->nheaptuples++;

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(vs->heapcxt);
}

/*
 * Scan the heap with a fresh MVCC snapshot and check that every visible tuple
 * is in the index, the way contrib/amcheck does for its heapallindexed check.
 */
static void
lion_verify_heapallindexed(LionVerifyState *vs)
{
	IndexInfo  *indexinfo = BuildIndexInfo(vs->index);
	TableScanDesc scan;
	Snapshot	snapshot;

	vs->heapcxt = AllocSetContextCreate(CurrentMemoryContext,
										"lion index verify heap tuple",
										ALLOCSET_DEFAULT_SIZES);

	snapshot = RegisterSnapshot(GetTransactionSnapshot());

	/*
	 * A new snapshot is guaranteed to have every entry the index needs, but
	 * an old transaction snapshot may predate the index's indcheckxmin
	 * horizon, in which case it is not safe to use.  That test - and the
	 * validity/readiness tests next to it - are the planner's, shared with
	 * the SQL count functions in lion_index_usable().
	 */
	{
		const char *why;

		if (!lion_index_usable(vs->index, snapshot, &why))
		{
			UnregisterSnapshot(snapshot);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot verify index \"%s\" against the heap because %s",
							RelationGetRelationName(vs->index), why)));
		}
	}

	/*
	 * The scan is created here rather than by table_index_build_scan() so
	 * that it really uses the snapshot registered above; the scan behaves
	 * like the first heap scan of a CREATE INDEX CONCURRENTLY, which maps
	 * heap-only tuples back to the TID of their HOT chain root - the TID the
	 * index actually holds.  table_index_build_scan() ends the scan for us.
	 */
	indexinfo->ii_Concurrent = true;
	indexinfo->ii_Unique = false;
	indexinfo->ii_ExclusionOps = NULL;
	indexinfo->ii_ExclusionProcs = NULL;
	indexinfo->ii_ExclusionStrats = NULL;

	scan = table_beginscan_strat(vs->heap, snapshot, 0, NULL, true, true);

	table_index_build_scan(vs->heap, vs->index, indexinfo, true, false,
						   lion_verify_heap_callback, (void *) vs, scan);

	UnregisterSnapshot(snapshot);
	MemoryContextDelete(vs->heapcxt);
	vs->heapcxt = NULL;
}

/*
 * lion_index_verify(regclass, heapallindexed bool)
 *
 * The locking and the change of identity are amcheck's
 * (amcheck_lock_relation_and_check()), for amcheck's reasons.
 *
 * THE TABLE IS LOCKED BEFORE THE INDEX, which is the order every command that
 * takes both follows (DROP INDEX, REINDEX, the executor).  Locking the index
 * first deadlocks with a transaction that has locked the table and goes on to
 * drop the index: it waits for our index lock while we wait for its table
 * lock.  The index's table is therefore looked up before either is locked,
 * and the lookup is repeated once both are held, because a concurrent DROP
 * INDEX and CREATE INDEX could have given the Oid to another index meanwhile.
 * A relation that is not an index has no table at all; opening it as one is
 * then what reports that.
 *
 * THE TABLE OWNER'S CODE RUNS AS THE TABLE OWNER.  With heapallindexed, the
 * heap scan evaluates the index's expressions and its predicate, and those
 * are functions the owner chose - and can replace after CREATE INDEX with
 * anything at all, IMMUTABLE label included.  Run as the caller, which is
 * normally a superuser checking somebody else's table, they would do whatever
 * the owner wrote with the superuser's rights (the class of CVE-2022-1552,
 * which amcheck and REINDEX fixed).  So everything from opening the index on
 * runs as the table owner, inside a SECURITY_RESTRICTED_OPERATION, with the
 * GUC changes those functions make confined to a nest level that is rolled
 * back when the check is over, and - on 17 and later, where the server's own
 * maintenance commands do the same - with search_path restricted to
 * pg_catalog and pg_temp.  On an ERROR the (sub)transaction abort restores
 * the identity and the settings; on the normal path this function does.
 *
 * The one decision that has to be the CALLER's is whether the error messages
 * may carry key values, so it is taken before the switch (showvalues).
 *
 * BOTH LOCKS ARE SHARELOCKS, which is what bt_index_parent_check() takes for
 * the same kind of check (see the file header): no writer and no VACUUM can
 * change the index while it is walked level by level, and none of the
 * whole-index comparisons can mistake a concurrent split for damage.  They
 * wait for the writers already in flight, block new ones until the check is
 * over, and are released when this function returns rather than at commit,
 * as amcheck does - nothing here sends an invalidation that could make that
 * unsafe.  During recovery only AccessShareLock is possible (and replay takes
 * no relation locks to be kept out by); DESIGN.md §7 says what that means.
 */
Datum
lion_index_verify(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		heapallindexed = PG_GETARG_BOOL(1);
	LOCKMODE	lockmode = RecoveryInProgress() ? AccessShareLock : ShareLock;
	LionVerifyState vs;
	Oid			heapoid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	memset(&vs, 0, sizeof(vs));
	vs.showvalues = superuser();

	heapoid = IndexGetRelation(relid, true);
	if (!OidIsValid(heapoid))
	{
		/*
		 * Not an index: opening it as one raises the error that says so.
		 * With AccessShareLock, because this is only to say that, and a
		 * ShareLock on a table would first wait for its writers.
		 */
		index_close(lion_open_index(relid, AccessShareLock), AccessShareLock);
		elog(ERROR, "could not find the table of index %u", relid);
	}
	vs.heap = table_open(heapoid, lockmode);

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(vs.heap->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	vs.index = lion_open_index(relid, lockmode);
	if (IndexGetRelation(relid, false) != heapoid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open the table of index \"%s\"",
						RelationGetRelationName(vs.index))));

	vs.ix = lion_get_index_state(vs.index);
	vs.nblocks = RelationGetNumberOfBlocks(vs.index);
	vs.cbuf = (LionContainer *) palloc(LION_CONTAINER_MAX_SIZE);
	vs.refs = (uint8 *) palloc0(sizeof(uint8) * Max(vs.nblocks, 1));

	lion_verify_meta(&vs);

	/*
	 * Test hook: the block count and the root are taken, nothing has been
	 * walked.  test/isolation/verify_concurrent.spec parks here and has a
	 * writer try to split the directory underneath.
	 */
	LION_INJECTION_POINT("lion-verify-meta-read");

	lion_verify_directory(&vs);
	lion_verify_reachable(&vs);

	if (heapallindexed)
		lion_verify_heapallindexed(&vs);

	pfree(vs.refs);
	pfree(vs.cbuf);

	/* Undo whatever settings the owner's functions changed, and ... */
	AtEOXact_GUC(false, save_nestlevel);
	/* ... be the caller again. */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	index_close(vs.index, lockmode);
	table_close(vs.heap, lockmode);

	PG_RETURN_VOID();
}
