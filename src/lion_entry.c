/*-------------------------------------------------------------------------
 *
 * lion_entry.c
 *		Entry tuples: making, rebuilding, finding and replacing them, and the
 *		cardinality guard of DESIGN.md §17.
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

/* ---------------------------------------------------------------------
 * Entry tuples
 * --------------------------------------------------------------------- */

/*
 * Fetch the item (container or sparse segment) at *off of an INLINE payload
 * into the aligned buffer buf and advance *off past it.  Returns the item
 * size, or 0 when the payload has been consumed.
 *
 * The payload may be followed by ZEROED SLACK: VACUUM writes a filtered
 * payload back into the bytes the entry already has rather than shrinking the
 * entry tuple, so that no other entry on the bucket page moves and the WAL
 * delta is the handful of bytes that changed (DESIGN.md §18).  No real item
 * has type 0 - the five item kinds are 1 .. 5 - so a zero item header is an
 * unambiguous end marker and the payload needs no length of its own.  The
 * same goes for a tail shorter than one header.
 */
Size
lion_inline_fetch(const char *payload, Size paylen, Size *off, LionContainer *buf)
{
	Size		avail;
	Size		peek;
	Size		csize;

	Assert(*off <= paylen);
	avail = paylen - *off;
	if (avail < LION_CONTAINER_HDRSZ)
		return 0;				/* end of the payload, or its zeroed slack */

	/*
	 * The item may be unaligned, so everything is read through the caller's
	 * buffer.  lion_container_size() of a RUN container needs the nruns field,
	 * which is the first uint16 of the payload, so peek that far before asking
	 * for the size.  Every other item kind, sparse segments included, is sized
	 * from the header alone.
	 */
	peek = Min(avail, LION_CONTAINER_HDRSZ + sizeof(uint16));
	memcpy(buf, payload + *off, peek);
	if (buf->type == 0)
		return 0;				/* the slack VACUUM left behind */
	if (!lion_container_type_valid(buf->type) && buf->type != LION_CT_SPARSE)
		elog(ERROR, "lion index: malformed inline item of type %u",
			 buf->type);
	/* a NARROW's width sizes it (DESIGN.md §38) */
	if (!lion_container_width_valid(buf))
		elog(ERROR, "lion index: malformed inline NARROW of width %u",
			 buf->flags);
	csize = lion_item_size(buf);
	if (csize < LION_CONTAINER_HDRSZ || csize > LION_CONTAINER_MAX_SIZE ||
		csize > avail)
		elog(ERROR, "lion index: malformed inline item");
	/* never stored, and it has no last container key (lion_sparse.h) */
	if (buf->type == LION_CT_SPARSE && buf->cardinality == 0)
		elog(ERROR, "lion index: empty sparse segment in an inline payload");

	memcpy(buf, payload + *off, csize);
	*off += csize;

	return csize;
}

/*
 * Build an entry tuple in palloc'd memory.
 *
 * The layout is: header, key bytes, MAXALIGN padding, payload.  For INLINE
 * entries the payload is a sequence of containers in ascending ckey order,
 * packed back to back with no padding, so a container inside it is generally
 * unaligned; read them with lion_inline_fetch().  head/tail/ncontainers/ntids
 * are left at their empty values; the caller fills them in.
 */
LionEntryTuple *
lion_make_entry(LionState *state, Datum key, uint32 hash, uint16 flags,
			   const char *payload, Size payloadlen, Size *size)
{
	LionEntryTuple *entry;
	Size		keylen = lion_key_datum_size(state, key);
	Size		payoff = MAXALIGN((LION_ENTRY_HDRSZ) + keylen);
	Size		total = payoff + payloadlen;

	if (total > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						total)));

	entry = (LionEntryTuple *) palloc0(total);
	entry->hash = hash;
	entry->flags = flags;
	entry->keylen = (uint16) keylen;
	entry->head = InvalidBlockNumber;
	entry->tail = InvalidBlockNumber;
	entry->ncontainers = 0;
	entry->attno = state->attno;
	entry->ntids = 0;

	lion_store_key(state, key, LionEntryGetKey(entry));

	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) entry) + payoff, payload, payloadlen);
	}

	*size = total;
	return entry;
}

/*
 * Build a reserved entry tuple: the NULL-key one (DESIGN.md §14) or the
 * no-key one a multi-key opclass needs for rows it extracts nothing from
 * (DESIGN.md §17).
 *
 * Neither has a key at all: keylen 0, hash 0, and the reservedflag bit, which
 * is how every reader recognises them.  The payload is an ordinary INLINE
 * payload, so once such an entry exists the insert, VACUUM and count paths
 * treat it exactly like any other.
 */
LionEntryTuple *
lion_make_reserved_entry(AttrNumber attno, uint16 reservedflag, uint16 flags,
						const char *payload, Size payloadlen, Size *size)
{
	LionEntryTuple *entry;
	Size		payoff = MAXALIGN(LION_ENTRY_HDRSZ);
	Size		total = payoff + payloadlen;

	Assert(reservedflag == LION_ENTRY_NULLKEY ||
		   reservedflag == LION_ENTRY_EMPTYKEY);
	Assert(attno >= 1);

	if (total > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						total)));

	entry = (LionEntryTuple *) palloc0(total);
	entry->hash = LION_NULLKEY_HASH;
	entry->flags = flags | reservedflag;
	entry->keylen = 0;
	entry->head = InvalidBlockNumber;
	entry->tail = InvalidBlockNumber;
	entry->ncontainers = 0;
	entry->attno = (uint16) attno;
	entry->ntids = 0;

	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) entry) + payoff, payload, payloadlen);
	}

	*size = total;
	return entry;
}

/*
 * Private copy of an entry tuple with a new payload.
 *
 * The header, the key bytes and their alignment padding are taken from entry
 * (which usually points into a page); payload replaces whatever payload the
 * original had.  A zero-length payload produces the shape of a CHAIN entry.
 * The caller owns the result and fills in flags, head, tail, ncontainers and
 * ntids as needed.
 */
/*
 * These two keep an entry's positions extension as it is, and so cannot
 * carry an INLINE positions section over: an entry that has one is rebuilt
 * by lion_entry_rebuild_pos(), which says what its positions become.
 */
static void
lion_entry_rebuild_check(const LionEntryTuple *entry)
{
	const LionEntryPosExt *x = lion_entry_posext(entry);

	if (x != NULL && x->pos_len != 0)
		elog(ERROR, "lion index: an entry with inline positions cannot be rebuilt without them");
}

LionEntryTuple *
lion_entry_rebuild(const LionEntryTuple *entry, const char *payload,
				  Size payloadlen, Size *size)
{
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		total = payoff + payloadlen;
	LionEntryTuple *copy;

	lion_entry_rebuild_check(entry);

	if (total > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						total)));

	copy = (LionEntryTuple *) palloc(total);
	memcpy(copy, entry, payoff);
	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) copy) + payoff, payload, payloadlen);
	}

	*size = total;
	return copy;
}

/*
 * The same, but with the entry ALLOCATED at `allocsz` bytes and everything
 * past the payload zeroed: growth slack for the next insert into this key
 * (DESIGN.md §4, applied to an INLINE payload).
 *
 * The slack needs no length field, because lion_inline_fetch() stops at the
 * first zero item header - no item kind is 0 - which is the convention
 * VACUUM's shrink-in-place already relies on (DESIGN.md §18).  So the payload
 * is self-terminating and every reader of it, the spill included, sees exactly
 * the items that are there.
 */
LionEntryTuple *
lion_entry_rebuild_slack(const LionEntryTuple *entry, const char *payload,
						 Size payloadlen, Size allocsz, Size *size)
{
	Size		payoff = LionEntryPayloadOffset(entry);
	LionEntryTuple *copy;

	lion_entry_rebuild_check(entry);
	Assert(allocsz >= payoff + payloadlen);

	if (allocsz > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						allocsz)));

	copy = (LionEntryTuple *) palloc0(allocsz);
	memcpy(copy, entry, payoff);
	if (payloadlen > 0)
	{
		Assert(payload != NULL);
		memcpy(((char *) copy) + payoff, payload, payloadlen);
	}

	*size = allocsz;
	return copy;
}

/*
 * A private copy of an entry that stores positions (LION_ENTRY_POSITIONS),
 * with a new payload AND a new positions section: header, key and extension
 * from entry, then the payload in a payload area of payarea bytes (at least
 * payloadlen; the rest is zeroed growth slack), then the poslen bytes of
 * positions at the next MAXALIGN boundary - so that the chunk there can be
 * read in place - with the extension's pos_len set to match.  A CHAIN shape
 * has neither; its pos_root is the caller's to set.
 */
LionEntryTuple *
lion_entry_rebuild_pos(const LionEntryTuple *entry, const char *payload,
					   Size payloadlen, Size payarea, const char *positions,
					   Size poslen, Size *size)
{
	Size		payoff = LionEntryPayloadOffset(entry);
	Size		posoff = (poslen > 0) ? MAXALIGN(payoff + payarea) :
		payoff + payarea;
	Size		total = posoff + poslen;
	LionEntryTuple *copy;

	Assert((entry->flags & LION_ENTRY_POSITIONS) != 0);
	Assert(payarea >= payloadlen);

	if (total > LION_MAX_ENTRY_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("lion index entry of %zu bytes is too large for a directory leaf",
						total)));

	copy = (LionEntryTuple *) palloc0(total);
	memcpy(copy, entry, payoff);
	if (payloadlen > 0)
		memcpy(((char *) copy) + payoff, payload, payloadlen);
	if (poslen > 0)
		memcpy(((char *) copy) + posoff, positions, poslen);
	lion_entry_posext(copy)->pos_len = (uint32) poslen;

	*size = total;
	return copy;
}

/*
 * shape - an entry built with no payload (lion_make_entry()) - as an entry
 * that stores positions: the flag, and an extension with no tree and no
 * inline positions.  Its size is its payload offset.
 */
LionEntryTuple *
lion_entry_add_posext(const LionEntryTuple *shape, Size *size)
{
	Size		keyend = LionEntryExtOffset(shape);
	LionEntryTuple *copy;
	LionEntryPosExt *x;

	Assert((shape->flags & LION_ENTRY_POSITIONS) == 0);
	copy = (LionEntryTuple *) palloc0(keyend + LION_ENTRY_POSEXT_SIZE);
	memcpy(copy, shape, keyend);
	copy->flags |= LION_ENTRY_POSITIONS;
	x = lion_entry_posext(copy);
	x->pos_root = InvalidBlockNumber;
	x->pos_len = 0;
	*size = LionEntryPayloadOffset(copy);
	return copy;
}

/*
 * Allocated length for an INLINE entry an INSERT is rewriting, so that the
 * next few members land inside the bytes it already has (DESIGN.md §4).
 *
 * The rule is the one items use - payload/8, clamped into
 * [LION_ITEM_SLACK_MIN, LION_ITEM_SLACK_MAX] - with two caps of its own.  The
 * slack is payload BYTES THAT HOLD NO PAYLOAD, so it may never push the stored
 * payload past `inline_limit`, and it may never be worth splitting the leaf
 * for: `maxsize` is the largest the entry may become without one, and the
 * caller works it out from the free space on the page.
 *
 * What this must NOT do is make a key spill early.  The spill test is made on
 * the REAL payload length before this is called, so an entry whose payload is
 * still inside inline_limit stays INLINE however much slack it carries; and an
 * entry whose payload has reached inline_limit gets none, because it is about
 * to spill anyway.  Only inserts add it: ambuild and VACUUM write entries at
 * the length they need (VACUUM keeps the length it finds, which is the same
 * slack seen from the other side).
 */
Size
lion_entry_alloc_size(Size payoff, Size paylen, Size inline_limit, Size maxsize)
{
	Size		extra;
	Size		alloc;

	extra = paylen / LION_ITEM_SLACK_FRACTION;
	extra = Max(extra, (Size) LION_ITEM_SLACK_MIN);
	extra = Min(extra, (Size) LION_ITEM_SLACK_MAX);
	extra = MAXALIGN(extra);

	if (paylen + extra > inline_limit)
		extra = (inline_limit > paylen) ? inline_limit - paylen : 0;

	alloc = payoff + paylen + extra;
	if (alloc > (Size) LION_MAX_ENTRY_SIZE)
		alloc = (Size) LION_MAX_ENTRY_SIZE;
	if (alloc > maxsize)
		alloc = maxsize;
	if (alloc < payoff + paylen)
		alloc = payoff + paylen;	/* no room for slack, or none wanted */

	Assert(alloc - (payoff + paylen) <= LION_ENTRY_SLACK_BOUND);
	return alloc;
}

/*
 * Find the entry for (hash, key) in the directory, comparing stored keys with
 * eqproc and ordering them with cmpproc (NULL for either means the index's
 * own).  DESIGN.md §21 replaced the bucket chain this used to walk with a
 * descent: on success *buf is a directory LEAF locked in lockmode, which the
 * caller releases, and *offnum is the entry's offset on it.
 *
 * On failure *buf is normally still a locked leaf - the one the key would go
 * on, which is what the insert path wants - but it is InvalidBuffer when the
 * cross-type fallback walk of lion_dir_find() ran, so a caller that uses it
 * must be one that never searches cross-type.
 */
bool
lion_find_entry_ext(Relation index, LionState *state, int lockmode, Datum key,
				   uint32 hash, FmgrInfo *eqproc, FmgrInfo *cmpproc,
				   Oid collation, Buffer *buf, OffsetNumber *offnum)
{
	LionSearchKey sk;

	lion_search_key_init(state, &sk, LION_KIND_VALUE, key, hash);
	if (eqproc != NULL)
	{
		sk.eqproc = eqproc;
		sk.cmpproc = cmpproc;
		sk.collation = collation;
	}

	return lion_dir_find(index, NULL, state->ix, &sk, lockmode, false,
						 buf, offnum, NULL);
}

bool
lion_find_entry(Relation index, LionState *state, int lockmode, Datum key,
			   uint32 hash, Buffer *buf, OffsetNumber *offnum)
{
	return lion_find_entry_ext(index, state, lockmode, key, hash, NULL, NULL,
							  InvalidOid, buf, offnum);
}

/*
 * Find a reserved entry (DESIGN.md §14 and §17).  Both sort before every real
 * key (LION_KIND_NULL and LION_KIND_EMPTY), so they live on the leftmost leaf
 * and are reached by the same descent as anything else.
 */
bool
lion_find_reserved_entry(Relation index, LionState *state, int lockmode,
						uint16 reservedflag, Buffer *buf, OffsetNumber *offnum)
{
	LionSearchKey sk;

	Assert(reservedflag == LION_ENTRY_NULLKEY ||
		   reservedflag == LION_ENTRY_EMPTYKEY);

	lion_search_key_init(state, &sk,
						 (reservedflag == LION_ENTRY_NULLKEY) ?
						 LION_KIND_NULL : LION_KIND_EMPTY,
						 (Datum) 0, LION_NULLKEY_HASH);

	return lion_dir_find(index, NULL, state->ix, &sk, lockmode, false,
						 buf, offnum, NULL);
}

/*
 * Replace the entry at (buf, offnum).  Returns false and changes nothing if
 * the new version does not fit.
 */
bool
lion_replace_entry(Relation index, LionWalState *state, Buffer buf,
				  OffsetNumber offnum, LionEntryTuple *entry, Size size)
{
	LionWalState *wal = state;
	Page		page;
	bool		ok;

	if (wal == NULL)
		wal = lion_wal_begin(index);

	page = lion_wal_register_buffer(wal, buf, LION_WALBUF_STD);

	/*
	 * PageIndexTupleOverwrite() tests before it writes, so a failure here has
	 * changed nothing - which is what lets an rmgr-mode record be abandoned
	 * at this point even though it is inside a critical section (DESIGN.md
	 * §25).
	 */
	lion_wal_save_item(wal, page, offnum);
	ok = PageIndexTupleOverwrite(page, offnum, entry, size);
	if (ok)
		lion_wal_op_replace(wal, page, offnum, entry, size);

	if (state == NULL)
	{
		if (ok)
			lion_wal_finish(wal, LION_XLOG_ENTRY);
		else
			lion_wal_abort(wal);
	}

	return ok;
}

/* ---------------------------------------------------------------------
 * Cardinality guard (DESIGN.md §17)
 *
 * The `max_entries` reloption is advisory: an index that grows past it keeps
 * working and keeps accepting rows, but says so once.  It exists because a
 * multi-key opclass makes it easy to index a column with millions of distinct
 * keys by accident (a tsvector of a whole document collection, an array of
 * UUIDs), and an inverted index with one entry per row is a slow way of
 * storing a table.
 * --------------------------------------------------------------------- */

/* Indexes this backend has already complained about, keyed by relation Oid. */
static HTAB *lion_warned_indexes = NULL;

int
lion_max_entries(Relation index)
{
	LionOptions *opts = (LionOptions *) index->rd_options;

	return opts ? opts->max_entries : LION_DEFAULT_MAX_ENTRIES;
}

void
lion_warn_max_entries(Relation index, int64 nentries)
{
	Oid			relid = RelationGetRelid(index);
	bool		found;

	if (lion_warned_indexes == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(Oid);
		ctl.hcxt = TopMemoryContext;
		lion_warned_indexes = hash_create("lion index max_entries warnings",
										 16, &ctl,
										 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	(void) hash_search(lion_warned_indexes, &relid, HASH_ENTER, &found);
	if (found)
		return;					/* this backend has said it once already */

	ereport(WARNING,
			(errmsg("lion index \"%s\" has more than %d distinct keys",
					RelationGetRelationName(index), lion_max_entries(index)),
			 errdetail("The index holds about " INT64_FORMAT " entries; its max_entries option is %d.",
					   nentries, lion_max_entries(index)),
			 errhint("Raise max_entries, or index a column with fewer distinct keys.")));
}
