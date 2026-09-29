/*-------------------------------------------------------------------------
 *
 * lion_funcs.c
 *		lion_index_stats(), lion_index_wal_mode() and lion_index_posting_root().
 *
 * Part of the SQL-callable helpers of the lion index; lion_funcs.h
 * describes them and declares what their files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_funcs.h"

PG_FUNCTION_INFO_V1(lion_index_stats);
PG_FUNCTION_INFO_V1(lion_index_posting_root);
PG_FUNCTION_INFO_V1(lion_index_wal_mode);

/*
 * Open relid as a lion index.
 */
Relation
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

	/*
	 * SUMMARY entries (DESIGN.md §32), which none of the counters above
	 * include: those describe the column's keys, and a summary is a second
	 * copy of some of their rows.  These say what that copy costs - its
	 * entries, the rows it holds, the bytes of its items and the posting pages
	 * of the summaries too large to stay INLINE.
	 */
	int64		summary_entries;
	int64		summary_tids;
	int64		summary_bytes;
	int64		summary_pages;
} LionColStats;

/* What one posting set contributed, before its column is known. */
typedef struct LionSetStats
{
	BlockNumber head;			/* hash key: the set's root block */
	uint16		attno;			/* 0 until the owning entry is seen */
	bool		summary;		/* ... and it is a summary's (§32) */
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
	LionMetaNdistinct nd;		/* the key counts on the meta page (§33) */
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
		ent->summary = false;
		memset(&ent->st, 0, sizeof(LionColStats));
	}
	return &ent->st;
}

/* ... and the same for an entry, whose column is known right away. */
static void
lion_stats_claim(LionStats *st, BlockNumber head, uint16 attno, bool summary)
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
	ent->summary = summary;
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

		/*
		 * A column's posting pages are its own for a single-column index -
		 * unless it has summaries (DESIGN.md §32), whose pages are counted
		 * apart and are known only through the entries that own them.
		 */
		if (st->ncolumns > 1 || ix->meta.summary_cols != 0)
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

					/* A summary (DESIGN.md §32) is counted apart. */
					if (LionEntryIsSummary(entry))
					{
						cs->summary_entries++;
						cs->summary_tids += (int64) entry->ntids;
						if ((entry->flags & LION_ENTRY_INLINE) != 0)
						{
							LionColStats tmp;
							Size		paylen = LION_ENTRY_PAYLOAD_LEN(entry,
																	   ItemIdGetLength(iid));
							Size		cur = 0;
							Size		csize;

							memset(&tmp, 0, sizeof(tmp));
							while ((csize = lion_inline_fetch(LionEntryGetPayload(entry),
															 paylen, &cur, cbuf)) > 0)
								lion_stats_item(&tmp, cbuf, csize);
							cs->summary_bytes += tmp.container_bytes;
						}
						else
							lion_stats_claim(st, entry->head, entry->attno,
											 true);
						continue;
					}

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
						lion_stats_claim(st, entry->head, entry->attno,
										 false);
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

				if (ent->summary)
				{
					cs->summary_bytes += ent->st.container_bytes;
					cs->summary_pages += ent->st.container_pages +
						ent->st.posting_internal_pages;
					continue;
				}

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

		/*
		 * The distinct keys the planner is given (DESIGN.md §33), as the last
		 * build, VACUUM or ANALYZE counted them: not this walk's entries, which
		 * count the NULL and EMPTY entries too and are as of now.
		 */
		(void) lion_read_meta_ndistinct(index, &st->nd);

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
		values[24] = Int64GetDatum(cs->summary_entries);
		values[25] = Int64GetDatum(cs->summary_tids);
		values[26] = Int64GetDatum(cs->summary_bytes);
		values[27] = Int64GetDatum(cs->summary_pages);
		if (call < LION_META_MAX_COLS &&
			(st->nd.valid_cols & (((uint32) 1) << call)) != 0)
			values[28] = Int64GetDatum((int64) st->nd.ndistinct[call]);
		else
			nulls[28] = true;

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
	 * Refused as the count functions refuse it (lion_count_sql.c).  The errors
	 * leave the index to the abort to close: its name is still needed.
	 */
	if (state->multikey)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("key column %d of index \"%s\" has a multi-key operator class, whose entries are not column values",
						1, RelationGetRelationName(index))));

	/*
	 * The key is looked up as one of the column's own values.  For a class
	 * declared on a polymorphic type (enum_ops is FOR TYPE anyenum) that is a
	 * value of the column's actual type, which the class's input type does
	 * not name: a domain over the enum is its enum, a different enum is not
	 * (lion_type_is_column() in lion_set.c makes the same test).
	 */
	if (OidIsValid(keytype) && keytype != index->rd_opcintype[0] &&
		!(IsPolymorphicType(index->rd_opcintype[0]) &&
		  getBaseType(keytype) == getBaseType(state->typid)))
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
