/*-------------------------------------------------------------------------
 *
 * lion_set.c
 *		Locating posting sets: key probes, lookups of one key or many, the list
 *		pin budget, and the lookup walk of keys that come one at a time.
 *
 * Part of the count engine: lion_count.h is its interface, and
 * lion_count_int.h declares what its files share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lion_count_int.h"

static uint32 lion_list_pin_budget(Relation index);
static void lion_list_pin_charge(LionPostingSet *ps);

/* ---------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */

/*
 * Oid of the "lion" access method.
 *
 * NOT cached in a static: the extension can be dropped and recreated inside
 * one backend (DROP EXTENSION ... CASCADE takes the indexes with it, so no
 * index survives to pin the old Oid), and the new pg_am row legitimately gets
 * a different Oid.  A process-local cache would then reject every index as
 * "not a lion index".  get_am_oid() is a GetSysCacheOid1(AMNAME) lookup,
 * which the syscache invalidates correctly and answers from memory.
 *
 * InvalidOid, not an error, where the access method does not exist: with the
 * library in shared_preload_libraries the planner hooks run in every
 * database, including those without CREATE EXTENSION pg_lion (template1 and
 * postgres, where pg_upgrade's own count(*) queries run), and there no index
 * or operator can match - which is what every caller does with InvalidOid.
 */
Oid
lion_get_am_oid(void)
{
	return get_am_oid("lion", true);
}

/*
 * Is a value of type `typid` one of this key column's own values?  Asked of a
 * class declared on a POLYMORPHIC type (enum_ops is FOR TYPE anyenum), whose
 * input type says nothing about which enum the column holds: the key type,
 * state->typid, does - the column's type as the index's tuple descriptor has
 * it (DESIGN.md §17's key type resolution).  Domains are looked through on
 * both sides, as the parser does: a domain over the enum has the enum's
 * representation, and the index's column may itself be of the domain.
 */
bool
lion_type_is_column(LionState *state, Oid typid)
{
	return OidIsValid(typid) &&
		getBaseType(typid) == getBaseType(state->typid);
}

/*
 * The type to name when a value cannot be compared with a key column: the
 * class's input type, or the column's actual type where that is polymorphic
 * ("the index is on type anyenum" says nothing about which one).  A multi-key
 * column's key type is the ELEMENT type (array_ops on text[] stores text), not
 * what the column holds, so there the class's type is named as before.
 */
Oid
lion_column_type(LionState *state, Oid opcintype)
{
	return (IsPolymorphicType(opcintype) && !state->multikey) ?
		state->typid : opcintype;
}

/*
 * LionProbe, the one cross-type resolution of DESIGN.md §21, is declared in
 * lion_count.h: the bitmap scan (lion_scan.c) resolves its scan keys through
 * lion_probe_init() and lion_probe_find() as well, so the two paths cannot
 * come to different conclusions about how to descend for a value.
 */
void
lion_probe_init(Relation index, LionState *state, Oid keytype, LionProbe *probe)
{
	/* The opclass of the KEY COLUMN this probe is for (DESIGN.md §24). */
	int			ci = state->attno - 1;
	Oid			opfamily = index->rd_opfamily[ci];
	Oid			opcintype = index->rd_opcintype[ci];
	Oid			eqopr;
	Oid			hashproc;
	Oid			cmpproc;
	Oid			sortproc;

	memset(probe, 0, sizeof(LionProbe));

	/*
	 * The column's OWN type: nothing to resolve.  That is the opclass's input
	 * type, or - for a class declared on a polymorphic type, enum_ops being
	 * FOR TYPE anyenum - the type the column actually has, whatever it is
	 * called.  A scan key names the class's member by its declared type
	 * (sk_subtype is anyenum), but the elements of `col = ANY (array)` are
	 * of the array's element type, which is the column's own enum: the
	 * operator is polymorphic, so the parser left the array as it was
	 * (make_scalar_array_op()).  Both are the class's own values, and looking
	 * up a cross-type (anyenum, mood) member for the second answered a plain
	 * index scan of `mood IN (...)` with "type mood cannot be compared with
	 * index" (2026-09-25 review).  lion_type_is_column() compares BASE types,
	 * because a domain over the enum is the enum's representation, and never
	 * accepts a different enum: its OIDs mean nothing to this column.
	 */
	if (!OidIsValid(keytype) || keytype == opcintype ||
		(IsPolymorphicType(opcintype) && lion_type_is_column(state, keytype)))
	{
		probe->typlen = state->typlen;
		probe->typbyval = state->typbyval;
		if (state->ordered)
		{
			probe->cmpproc = state->cmpproc;
			probe->hascmp = true;
			probe->sortproc = state->cmpproc;
			probe->hassort = true;
		}
		/* sorted by the directory's comparison, or by the hash it leads with */
		probe->walk = true;
		return;
	}

	eqopr = get_opfamily_member(opfamily, opcintype, keytype, 1);
	if (!OidIsValid(eqopr))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type %s cannot be compared with index \"%s\"",
						format_type_be(keytype),
						RelationGetRelationName(index)),
				 errdetail("The index is on type %s.",
						   format_type_be(lion_column_type(state, opcintype)))));

	hashproc = get_opfamily_proc(opfamily, keytype, keytype, 1);
	if (!OidIsValid(hashproc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("missing support function 1 for type %s in operator family \"%s\"",
						format_type_be(keytype),
						get_opfamily_name(opfamily, false))));

	fmgr_info(get_opcode(eqopr), &probe->eqproc);
	fmgr_info(hashproc, &probe->hashinfo);
	probe->crosstype = true;
	get_typlenbyval(keytype, &probe->typlen, &probe->typbyval);

	/*
	 * An UNORDERED directory is in (kind, hash, bytes) order whatever the
	 * family offers: no comparison may be used on it, the family's cross-type
	 * proc 4 included, because descending a hash-ordered tree in value order
	 * finds nothing (§21 rule 3 can leave a column unordered although its
	 * family names comparisons).  The value's own hash is the family's
	 * cross-type hash, which agrees with the stored keys' by the family's
	 * contract, so a descent - and a list sorted by hash - is exact.
	 */
	if (!state->ordered)
	{
		probe->walk = true;
		return;
	}

	/*
	 * The family's cross-type ordering (proc 4 for the pair): descend as
	 * usual.  A LIST of such values must be sorted into the directory order
	 * before it can be walked, and the cross-type function cannot compare two
	 * values of the search type with each other.  The family's own proc 4 for
	 * (keytype, keytype) can: it is the family's statement of how it orders
	 * that type, the same promise that makes its cross-type proc 4 usable at
	 * all.  The search type's DEFAULT btree order is not used: it is the
	 * directory's order only when the opclass happens to sort that way (a
	 * reverse comparison does not), and a list sorted against the direction
	 * of the leaves loses every value but the first to the walk, which only
	 * steps right.  Without a comparison of its own the list is not walked:
	 * every value descends by itself.
	 */
	cmpproc = get_opfamily_proc(opfamily, opcintype, keytype, LION_CMP_PROC);
	if (OidIsValid(cmpproc))
	{
		fmgr_info(cmpproc, &probe->cmpproc);
		probe->hascmp = true;

		sortproc = get_opfamily_proc(opfamily, keytype, keytype,
									 LION_CMP_PROC);
		if (OidIsValid(sortproc))
		{
			fmgr_info(sortproc, &probe->sortproc);
			probe->hassort = true;
			probe->walk = true;
		}
		return;
	}

	/*
	 * The tree is ordered by a comparison this value cannot take part in.  A
	 * BINARY coercion to the key type - the same bytes, varchar to text -
	 * makes it one of the index's own values, and then hash, equality and
	 * ordering are all the index's own.  That is only correct if the family's
	 * cross-type EQUALITY is that same predicate: a family may declare
	 * `text = bpchar` through its own function with different semantics
	 * ('x' vs 'x ' differ as text, agree as bpchar), and then relabelling the
	 * probe would silently swap the family's equality for the key type's.  So
	 * the shortcut is taken only when the family's cross-type strategy-1
	 * operator is implemented by the SAME function as the key type's own
	 * strategy-1 operator - the two predicates are then one function applied
	 * to the same bytes.  A cast FUNCTION is never taken, implicit or not:
	 * implicit does not mean lossless (text -> name truncates to 63 bytes),
	 * and PostgreSQL has no way to say that a cast is a bijection.  Anything
	 * else keeps the family's cross-type equality and walks the leaves.
	 */
	{
		Oid			castfunc = InvalidOid;
		Oid			sameeq = get_opfamily_member(opfamily, opcintype, opcintype, 1);

		if (find_coercion_pathway(opcintype, keytype, COERCION_IMPLICIT,
								  &castfunc) == COERCION_PATH_RELABELTYPE &&
			OidIsValid(sameeq) &&
			get_opcode(sameeq) == get_opcode(eqopr))
		{
			/* From here on the probe values ARE the index's own type. */
			probe->coerce = true;
			probe->crosstype = false;
			probe->typlen = state->typlen;
			probe->typbyval = state->typbyval;
			probe->cmpproc = state->cmpproc;
			probe->hascmp = true;
			probe->sortproc = state->cmpproc;
			probe->hassort = true;
			probe->walk = true;
			return;
		}
	}

	/*
	 * Neither: the leaves are walked with the family's cross-type EQUALITY
	 * (lion_dir_find()), per value.  Correct and linear.
	 */
	probe->needscan = true;
}

/*
 * The Datum to probe with: the caller's value, which a binary coercion to the
 * key type (the only one lion_probe_init() takes) leaves as it is.
 */
static inline Datum
lion_probe_value(LionProbe *probe, Datum value)
{
	return value;				/* a binary coercion needs no work at all */
}

/* A search key for one probe value. */
static void
lion_probe_search_key(LionState *state, LionProbe *probe, Datum key,
					 uint32 hash, LionSearchKey *sk)
{
	lion_search_key_init(state, sk, LION_KIND_VALUE, key, hash);
	if (probe->crosstype)
	{
		sk->eqproc = &probe->eqproc;
		sk->cmpproc = probe->hascmp ? &probe->cmpproc : NULL;
	}
}

static inline uint32
lion_probe_hash(LionState *state, LionProbe *probe, Datum key)
{
	if (!probe->crosstype)
		return lion_hash_key(state, key);
	return DatumGetUInt32(FunctionCall1Coll(&probe->hashinfo, state->collation,
											key));
}

/*
 * Locate the entry of one value through a resolved probe: lion_dir_find()
 * with the search key lion_probe_init() decided on.  On true *buf is the leaf
 * locked in lockmode and *off the entry; on false *buf may be a locked leaf
 * or InvalidBuffer, and the caller releases it when it is valid.
 */
bool
lion_probe_find(Relation index, LionState *state, LionProbe *probe,
				Datum value, int lockmode, Buffer *buf, OffsetNumber *off)
{
	LionSearchKey sk;
	uint32		hash;

	value = lion_probe_value(probe, value);
	hash = lion_probe_hash(state, probe, value);
	lion_probe_search_key(state, probe, value, hash, &sk);

	return lion_dir_find(index, NULL, state->ix, &sk, lockmode, false,
						 buf, off, NULL);
}


/* ---------------------------------------------------------------------
 * Locating posting sets
 * --------------------------------------------------------------------- */

/*
 * Fill *ps from the entry tuple at (buf, offnum), which the caller holds
 * locked SHARE.  Returns with the lock still held; the caller decides what to
 * do with the buffer.  When the entry is INLINE the payload is copied out and
 * *keeppin is set: the caller must keep a pin on buf and store it in
 * ps->pinbuf (DESIGN.md section 9).
 */

static void
lion_fill_posting_set(Relation index, LionState *state, Buffer buf,
					 OffsetNumber offnum, LionPostingSet *ps, bool *keeppin)
{
	Page		page = BufferGetPage(buf);
	ItemId		iid = PageGetItemId(page, offnum);

	lion_fill_posting_set_entry(index, state,
								(LionEntryTuple *) PageGetItem(page, iid),
								ItemIdGetLength(iid), BufferGetBlockNumber(buf),
								offnum, ps, keeppin);
}

/*
 * The posting set of the entry at (buf, offnum), which the caller holds
 * locked: an INLINE one keeps a pin of its own on the leaf, as a located set
 * does (DESIGN.md §9), so the caller may let the leaf go.
 */
void
lion_posting_set_at(Relation index, LionState *state, Buffer buf,
					OffsetNumber offnum, LionPostingSet *ps)
{
	bool		keeppin;

	lion_fill_posting_set(index, state, buf, offnum, ps, &keeppin);
	if (keeppin)
	{
		IncrBufferRefCount(buf);
		ps->pinbuf = buf;
	}
}

/*
 * The same from an entry tuple of itemsz bytes that was at (blkno, offnum): a
 * leaf the caller holds locked, or a private copy of one taken under that lock
 * while the caller still holds the leaf's pin (lion_entry_scan_next()), which
 * is the pin *keeppin asks it to hand over.
 */
void
lion_fill_posting_set_entry(Relation index, LionState *state,
							const LionEntryTuple *entry, Size itemsz,
							BlockNumber blkno, OffsetNumber offnum,
							LionPostingSet *ps, bool *keeppin)
{
	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = entry->attno;
	ps->pinbuf = InvalidBuffer;
	ps->found = true;
	ps->ntids = entry->ntids;
	ps->ncontainers = entry->ncontainers;
	ps->entryblk = blkno;
	ps->entryoff = offnum;
	ps->cxt = CurrentMemoryContext;
	ps->nuses = 0;
	ps->mat = NULL;
	ps->matfailed = false;

	/*
	 * lion_fetch_key() points into the page for by-reference types, so copy
	 * the key out while the buffer is still locked.  The reserved NULL entry
	 * (DESIGN.md §14) has no key bytes to copy.
	 */
	ps->keyisnull = LionEntryIsNullKey(entry);
	if (ps->keyisnull)
	{
		ps->storedkey = (Datum) 0;
		ps->hasstoredkey = true;
	}
	else
	{
		ps->storedkey = datumCopy(lion_fetch_key(state, LionEntryGetKey(entry)),
								  state->typbyval, state->typlen);
		ps->hasstoredkey = true;
	}

	if ((entry->flags & LION_ENTRY_INLINE) != 0)
	{
		ps->is_inline = true;
		ps->head = InvalidBlockNumber;
		ps->paylen = LION_ENTRY_PAYLOAD_LEN(entry, itemsz);
		if (ps->paylen > 0)
		{
			ps->payload = (char *) palloc(ps->paylen);
			memcpy(ps->payload, LionEntryGetPayload(entry), ps->paylen);
		}
		*keeppin = true;
	}
	else
	{
		Assert((entry->flags & LION_ENTRY_CHAIN) != 0);
		ps->is_inline = false;
		ps->head = entry->head;
		*keeppin = false;
	}
}

/*
 * One key of an IN list, in the order its entry is looked up in: the
 * DIRECTORY order (DESIGN.md §21), so that the whole list is located in one
 * left-to-right walk of the leaves and duplicates - which sort together - are
 * dropped by looking at the neighbours.
 */
typedef struct LionProbeKey
{
	uint32		hash;
	int32		idx;			/* position in the caller's value array */
} LionProbeKey;

typedef struct LionProbeSort
{
	const Datum *values;
	LionProbe  *probe;
	Oid			collation;
	uint32		ncmp;			/* comparisons made by the sort */
} LionProbeSort;

static int
lion_probe_key_cmp(const void *a, const void *b, void *arg)
{
	const LionProbeKey *x = (const LionProbeKey *) a;
	const LionProbeKey *y = (const LionProbeKey *) b;
	LionProbeSort *ctx = (LionProbeSort *) arg;

	if (ctx->probe->hassort)
	{
		int32		c = DatumGetInt32(FunctionCall2Coll(&ctx->probe->sortproc,
														ctx->collation,
														ctx->values[x->idx],
														ctx->values[y->idx]));

		if (c != 0)
			return c < 0 ? -1 : 1;
	}
	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

/*
 * The same, as the sort of a whole list calls it: a list whose length is a
 * parameter's has no cap (DESIGN.md §15), its sort calls the opclass's
 * comparison some n log n times, and nothing else in it would answer a
 * cancel.  No lock is held while a list is sorted.
 */
static int
lion_probe_sort_cmp(const void *a, const void *b, void *arg)
{
	LionProbeSort *ctx = (LionProbeSort *) arg;

	if ((++ctx->ncmp & 0xffff) == 0)
		CHECK_FOR_INTERRUPTS();
	return lion_probe_key_cmp(a, b, arg);
}

/*
 * Sort the non-NULL values of a list into the order a lookup locates them in
 * (lion_probe_key_cmp(): the probe's own comparison, then the hash), and hand
 * back each one's hash.  Returns how many there are.  Two values of one
 * equality class compare equal and hash alike, so they come out adjacent and
 * with equal hashes: a caller that cuts the list only where the hash changes
 * never splits a class between two pieces, which is what lets a plain scan
 * locate a long list piece by piece without ever returning an entry twice
 * (DESIGN.md §29.4).
 */
int
lion_probe_sort(Relation index, AttrNumber attno, Oid keytype, int nvalues,
				const Datum *values, const bool *isnull, Datum *sorted,
				uint32 *hashes)
{
	LionState   *state = lion_index_column_state(index, attno);
	LionProbe	probe;
	LionProbeSort sortctx;
	LionProbeKey *probes;
	int			n = 0;
	int			i;

	lion_probe_init(index, state, keytype, &probe);
	probes = (LionProbeKey *) palloc_extended(sizeof(LionProbeKey) *
											  Max(nvalues, 1),
											  MCXT_ALLOC_HUGE);
	for (i = 0; i < nvalues; i++)
	{
		if (isnull != NULL && isnull[i])
			continue;
		probes[n].hash = lion_probe_hash(state, &probe, values[i]);
		probes[n].idx = i;
		n++;
		if ((n & 0x3ff) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	sortctx.values = values;
	sortctx.probe = &probe;
	sortctx.collation = state->collation;
	sortctx.ncmp = 0;
	if (n > 1)
		qsort_arg(probes, n, sizeof(LionProbeKey), lion_probe_sort_cmp,
				  &sortctx);

	for (i = 0; i < n; i++)
	{
		sorted[i] = values[probes[i].idx];
		hashes[i] = probes[i].hash;
	}
	pfree(probes);
	return n;
}

/*
 * Fill *ps from the entry the caller has located at (buf, offnum), which is a
 * directory leaf held SHARE, and release the buffer - keeping its pin when the
 * entry is INLINE, because that pin is the DESIGN.md §9 interlock.
 *
 * Always: one lookup is one pin, and what a caller's loop of them holds is
 * bounded by the query, not by the data (DESIGN.md §15).
 */
static void
lion_posting_set_take(Relation index, LionState *state, Buffer buf,
					 OffsetNumber offnum, LionPostingSet *ps)
{
	bool		keeppin;

	lion_fill_posting_set(index, state, buf, offnum, ps, &keeppin);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	if (keeppin)
		ps->pinbuf = buf;
	else
		ReleaseBuffer(buf);
}

/*
 * Locate the entry of one key whose hash has already been computed.  This is
 * lion_posting_set_lookup() from the descent onwards, split out so that a
 * whole IN list can be sorted before any page is read
 * (lion_posting_set_lookup_many()).
 */
static bool
lion_posting_set_locate(Relation index, LionState *state, LionProbe *probe,
					   Datum key, uint32 hash, LionPostingSet *ps)
{
	LionSearchKey sk;
	Buffer		buf;
	OffsetNumber off;

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = state->attno;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;
	ps->entryblk = InvalidBlockNumber;
	ps->entryoff = InvalidOffsetNumber;

	lion_probe_search_key(state, probe, key, hash, &sk);

	if (!lion_dir_find(index, NULL, state->ix, &sk, BUFFER_LOCK_SHARE, false,
					   &buf, &off, NULL))
	{
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
		return false;
	}

	lion_posting_set_take(index, state, buf, off, ps);
	return true;
}

bool
lion_posting_set_lookup_col(Relation index, AttrNumber attno, Datum key,
						   Oid keytype, LionPostingSet *ps)
{
	LionState   *state = lion_index_column_state(index, attno);
	LionProbe	probe;
	uint32		hash;

	lion_probe_init(index, state, keytype, &probe);
	key = lion_probe_value(&probe, key);
	hash = lion_probe_hash(state, &probe, key);

	return lion_posting_set_locate(index, state, &probe, key, hash, ps);
}


/*
 * One of many single lookups a caller keeps located together - the keys of a
 * multi-key clause, as many as LION_MAX_QUERY_KEYS of them, and one clause
 * per `@>` or `@@` of the query - under the list pin budget (DESIGN.md §15,
 * "The pin budget").  They used to keep every INLINE leaf pin whatever the
 * budget said, which a clause whose query is a parameter (DESIGN.md §17, "A
 * query known only at run time") sets to a thousand pins a clause with
 * nothing in the query text to show for it (2026-09-28 review).
 *
 * The lookup is lion_posting_set_lookup_col()'s; only what the set keeps
 * differs.  A pin on the leaf the caller's previous set took (*lastpinned)
 * costs no buffer and is not charged, as in a list; a pin on another leaf is
 * charged while the budget lasts, and past it the set lets its leaf go and
 * comes out NOPIN, which every count copes with (lion_count_sources_run()).
 * The keys are looked up in the order the query names them, not in the
 * directory's, so the same leaf may be charged twice: the budget runs out
 * early, never late.
 */
bool
lion_posting_set_lookup_budgeted_col(Relation index, AttrNumber attno,
									 Datum key, Oid keytype,
									 LionPostingSet *ps, Buffer *lastpinned)
{
	bool		found;

	found = lion_posting_set_lookup_col(index, attno, key, keytype, ps);
	if (!BufferIsValid(ps->pinbuf))
		return found;			/* not found, or a CHAIN set: no leaf kept */

	if (ps->pinbuf == *lastpinned)
		return found;
	if (lion_list_pin_budget(index) == 0)
	{
		lion_posting_set_unpin(ps);
		return found;
	}
	lion_list_pin_charge(ps);
	*lastpinned = ps->pinbuf;
	return found;
}

/*
 * How many leaves a walk may step right over before it is cheaper to descend
 * again (DESIGN.md §21).  A dense list steps; a list of a handful of values
 * spread over the whole index descends, instead of reading every leaf in
 * between.
 */
#define LION_LOOKUP_WALK_MAX	8

/*
 * THE LIST PIN BUDGET (DESIGN.md §15): how many distinct leaves the IN lists
 * this backend has located and not yet released may keep pinned, all of them
 * together.  It is the smallest of
 *
 *	- LION_LOOKUP_MAX_PINS, 1000: the longest list the planner accepts as a
 *	  literal, so that a literal list pins exactly what it always did - a set
 *	  that is NOPIN costs a second descent in the disjoint sum and the visibility
 *	  map in an OR, and an ordinary query must not pay either;
 *	- an EIGHTH of the buffer pool the index is read into, so that one
 *	  backend never pins more than a modest fraction of the pool however many
 *	  lists its query has.  The failure this budget exists for was 9000 leaves
 *	  against a 2048-buffer pool (16MB): the query ran itself out of buffers,
 *	  and one short of that it would have starved everyone else.  With an
 *	  eighth, 256 there, the query keeps seven eighths for its own heap,
 *	  visibility-map and chain pages and for every other backend.  It only
 *	  binds below 64MB of shared_buffers.  The pool of a TEMPORARY index is
 *	  the backend's own local buffers, temp_buffers (1024 by default, and as
 *	  few as 100), which nothing else shares and which run out just the same:
 *	  "no empty local buffer available" (lion_pin_pool());
 *	- for the SHARED pool, LION_LOOKUP_SHARES times the backend's fair share
 *	  of it, NBuffers / MaxBackends, but never less than LION_LOOKUP_MIN_PINS
 *	  (2026-09-28 review).  The eighth keeps one backend from taking the
 *	  pool, but eight backends running such lists at once still took all of
 *	  it; with the share, the backends that must do so are a sixteenth of all
 *	  the server is configured for, however many that is.  On a stock server
 *	  (128MB, 100 connections: some 130 buffers a backend) the share is 2000
 *	  and the thousand binds, so an ordinary configuration pins what it always
 *	  did; the share binds where max_connections is large for shared_buffers.
 *
 * and a participant of a parallel plan gets its SHARE of that: the limit
 * divided by the participants the plan was started with
 * (lion_list_pin_participants()).  Each participant of a parallel FK-side
 * join locates the fact filters for itself (DESIGN.md §27), and a leader with
 * seven workers took eight budgets for one query.
 *
 * None of them is the backend's plain "fair share" (GetAdditionalPinLimit()
 * of 18): 86 buffers on a stock 128MB server, and a budget of it sent
 * ordinary thousand-value lists to the heap.  No bound kept per backend can
 * promise the pool to every backend at once - that needs a count in shared
 * memory, which an extension that need not be preloaded does not have - and a
 * set past the budget only costs time: it comes out NOPIN, never wrong.
 *
 * The count is backend-wide because the budget is: two unbounded lists in one
 * query share it instead of taking one budget each.  Every set that took a
 * NEW leaf for it is marked `budgeted` and gives it back when released or
 * unpinned.  A set abandoned by an error is never released - but its pin is,
 * by the resource owner that was current when it was taken, and that owner is
 * released with the transaction, subtransaction or portal the error ends.  So
 * each pin is charged to its OWNER as well (lion_list_pin_charge()), and an
 * owner that is released gives back what its sets still had charged
 * (lion_list_pins_resowner()).  A subtransaction failing in a loop - a
 * PL/pgSQL EXCEPTION block around a count - used to leave its charges behind
 * until the top-level transaction ended, and every list after it came out
 * NOPIN (2026-09-28 review); now they go when its pins do, while the sets of
 * a portal that outlives it (a cursor FETCHed inside it) keep theirs, as they
 * keep their pins.  The count is still zeroed at the end of every top-level
 * transaction, when no set can be left.
 *
 * The CURSORS that read the located sets draw on what is left of the same
 * limit (lion_open_budget_init()): a CHAIN set pins the posting page its
 * current container came from, and a list of CHAIN entries located no leaf
 * pin at all and then pinned a page per value when its cursors were built.
 */
#define LION_LOOKUP_MAX_PINS	1000
#define LION_LOOKUP_SHARES		16
#define LION_LOOKUP_MIN_PINS	64

static uint32 lion_list_pins = 0;
static bool lion_list_pins_cb = false;
static int	lion_list_participants = 1;
static ResourceOwner lion_list_participants_owner = NULL;

/*
 * The pins charged to one resource owner.  There are as many of these as
 * owners that hold list pins at one time - the portal a count runs in, a
 * cursor's, a function's - which is a handful, so they are an array searched
 * from its end.
 */
typedef struct LionPinCharge
{
	ResourceOwner owner;
	uint32		pins;
} LionPinCharge;

static LionPinCharge *lion_pin_charges = NULL;
static int	lion_pin_ncharges = 0;
static int	lion_pin_chargecap = 0;

/*
 * The buffer pool a relation's pages are pinned in: the backend's local
 * buffers for a temporary relation, shared_buffers otherwise.  temp_buffers
 * cannot change once the session has touched a temporary table, so the
 * setting is the pool.
 */
static int
lion_pin_pool(Relation rel)
{
	if (rel != NULL && RelationUsesLocalBuffers(rel))
		return num_temp_buffers;
	return NBuffers;
}

static uint32
lion_pin_limit(Relation rel)
{
	uint32		limit;

	limit = (uint32) Min(LION_LOOKUP_MAX_PINS, Max(lion_pin_pool(rel) / 8, 1));
	if (rel == NULL || !RelationUsesLocalBuffers(rel))
	{
		int64		share;

		share = (int64) LION_LOOKUP_SHARES * NBuffers / Max(MaxBackends, 1);
		share = Max(share, (int64) LION_LOOKUP_MIN_PINS);
		limit = (uint32) Min((int64) limit, share);
	}
	if (lion_list_participants > 1)
		limit = Max(limit / (uint32) lion_list_participants, (uint32) 1);
	return limit;
}

/*
 * Set by the count pushdown when a parallel plan starts (DESIGN.md §27): how
 * many participants it was started with, the leader included, each of which
 * locates lists of its own.  Back to 1 when the leader's node ends, and at
 * the end of every top-level transaction.
 */
void
lion_list_pin_participants(int participants)
{
	lion_list_participants = Max(participants, 1);

	/*
	 * An ERROR in a subtransaction ends the query without the node's End, so
	 * the share is also given back when the owner the node ran under is
	 * released (lion_list_pins_resowner()), and the callbacks that do it must
	 * exist by then.
	 */
	lion_list_participants_owner =
		(lion_list_participants > 1) ? CurrentResourceOwner : NULL;
	(void) lion_list_pin_budget(NULL);
}

static void
lion_list_pins_xact(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			lion_list_pins = 0;
			lion_pin_ncharges = 0;
			lion_list_participants = 1;
			lion_list_participants_owner = NULL;
			break;
		default:
			break;
	}
}

/*
 * A resource owner is being released - a portal's, a subtransaction's, the
 * transaction's - and with it every buffer pin it still holds, among them the
 * pins of the sets an error abandoned: their charges go with them.  The
 * callback runs with CurrentResourceOwner set to the owner being released,
 * once per phase.  A set charged to it that is released after all finds no
 * charge left and gives nothing back (lion_list_pin_return()).
 */
static void
lion_list_pins_resowner(ResourceReleasePhase phase, bool isCommit,
						bool isTopLevel, void *arg)
{
	int			i;

	if (phase != RESOURCE_RELEASE_BEFORE_LOCKS)
		return;

	/* The parallel query that divided the budget is over, one way or another. */
	if (lion_list_participants_owner != NULL &&
		lion_list_participants_owner == CurrentResourceOwner)
	{
		lion_list_participants = 1;
		lion_list_participants_owner = NULL;
	}

	if (lion_pin_ncharges == 0)
		return;
	for (i = lion_pin_ncharges - 1; i >= 0; i--)
	{
		if (lion_pin_charges[i].owner != CurrentResourceOwner)
			continue;
		lion_list_pins -= Min(lion_list_pins, lion_pin_charges[i].pins);
		lion_pin_charges[i] = lion_pin_charges[--lion_pin_ncharges];
		break;
	}
}

static uint32
lion_list_pin_budget(Relation index)
{
	uint32		limit = lion_pin_limit(index);

	if (!lion_list_pins_cb)
	{
		RegisterXactCallback(lion_list_pins_xact, NULL);
		RegisterResourceReleaseCallback(lion_list_pins_resowner, NULL);
		lion_list_pins_cb = true;
	}
	return (lion_list_pins < limit) ? limit - lion_list_pins : 0;
}

/*
 * The open budget of one count, or of one bitmap walk (DESIGN.md §15,
 * "Bounded cursors").
 *
 *	mem		work_mem: the executor's answer to "how much may one node keep",
 *			which is what the recheck batch and the visibility cache are
 *			bounded by as well.  Each is a separate allowance, as the inputs
 *			of a hash join each get one.
 *	pins	what the lists this backend has located have left of the list pin
 *			budget above, so that the leaves a list keeps pinned and the
 *			pages its cursors pin come out of ONE limit per backend - an
 *			eighth of the pool, at most a thousand buffers.
 *
 * rel names the pool (lion_pin_pool()): the heap for a count, whose indexes
 * share its persistence, the index for a bitmap walk.
 */
void
lion_open_budget_init(LionOpenBudget *budget, Relation rel)
{
	uint32		limit = lion_pin_limit(rel);
	uint32		left = (lion_list_pins < limit) ? limit - lion_list_pins : 0;

	budget->mem = Max((Size) work_mem * 1024, (Size) LION_OPEN_MIN_BYTES);
	budget->pins = (int) Max(left, (uint32) LION_OPEN_MIN_PINS);
}

/*
 * A set takes a leaf of the budget.  The pin it has just taken belongs to the
 * current resource owner, and so does the charge.  The callbacks are
 * registered by lion_list_pin_budget(), which every caller has asked first.
 */
static void
lion_list_pin_charge(LionPostingSet *ps)
{
	ResourceOwner owner = CurrentResourceOwner;
	int			i;

	Assert(lion_list_pins_cb);
	for (i = lion_pin_ncharges - 1; i >= 0; i--)
	{
		if (lion_pin_charges[i].owner == owner)
			break;
	}
	if (i < 0)
	{
		if (lion_pin_ncharges >= lion_pin_chargecap)
		{
			int			newcap = Max(lion_pin_chargecap * 2, 8);

			if (lion_pin_charges == NULL)
				lion_pin_charges = (LionPinCharge *)
					MemoryContextAlloc(TopMemoryContext,
									   sizeof(LionPinCharge) * newcap);
			else
				lion_pin_charges = (LionPinCharge *)
					repalloc(lion_pin_charges, sizeof(LionPinCharge) * newcap);
			lion_pin_chargecap = newcap;
		}
		i = lion_pin_ncharges++;
		lion_pin_charges[i].owner = owner;
		lion_pin_charges[i].pins = 0;
	}
	lion_pin_charges[i].pins++;
	lion_list_pins++;
	ps->budgeted = true;
	ps->pinowner = owner;
}

/*
 * A set that took a leaf of the budget gives it back - to the owner it was
 * charged to, unless that owner has been released since and given it back
 * already.
 */
static inline void
lion_list_pin_return(LionPostingSet *ps)
{
	int			i;

	if (!ps->budgeted)
		return;
	ps->budgeted = false;
	for (i = lion_pin_ncharges - 1; i >= 0; i--)
	{
		if (lion_pin_charges[i].owner != ps->pinowner)
			continue;
		if (lion_pin_charges[i].pins > 0)
			lion_pin_charges[i].pins--;
		if (lion_list_pins > 0)
			lion_list_pins--;
		if (lion_pin_charges[i].pins == 0)
			lion_pin_charges[i] = lion_pin_charges[--lion_pin_ncharges];
		break;
	}
	ps->pinowner = NULL;
}

/*
 * Locate the posting sets of many keys of one index at once: the IN list of
 * DESIGN.md §15, whose union the merge in this file then evaluates.
 *
 * Two things are done here that a loop over lion_posting_set_lookup() cannot:
 *
 *	- the values are sorted into the DIRECTORY order first and the leaves are
 *	  then walked left to right, stepping right while the next value is only a
 *	  few pages ahead and descending again when it is further, so a thousand
 *	  values cost one pass over the leaves they live on instead of a thousand
 *	  random page reads (DESIGN.md §21);
 *	- duplicates are dropped in one pass over that order instead of by
 *	  comparing every value with every earlier one, which at the 1000 values
 *	  the planner allows is half a million datumIsEqual() calls.
 *
 * Duplicates are dropped TWICE OVER, and the second pass is the one that
 * matters (DESIGN.md §15).  The bytewise comparison comes first because it is
 * free and saves the lookup, but it is not exhaustive: an opclass whose
 * equality is not byte equality - citext - has distinct values that reach ONE
 * entry.  So every located entry's STORED KEY is also compared, with the
 * index's own equality, against the ones the same run has already found, and
 * a repeat is released again.  A union would not have cared (a set ORed with
 * itself is that set); the disjoint-SUM short-circuit does, because it would
 * add the entry's rows twice.
 *
 * *sets must have room for nvalues sets; the located ones come out packed at
 * the front, in key order, and the return value is how many there are.  Every
 * one of them - found or not - must be handed to lion_posting_set_release().
 * *nfound, if given, is how many of them have an entry in the index at all:
 * nfound == 0 means the union selects nothing.
 *
 * The pins are BUDGETED (DESIGN.md §15, 2026-09-23 review).  Each INLINE set
 * keeps a pin on the leaf its payload was copied from, the §9 interlock, and
 * nothing bounded how many leaves that came to: an array parameter over an
 * index with more leaves than shared_buffers ran out of buffers.  So at most
 * what is left of the backend's list pin budget (above) - and the INLINE
 * sets found past that come out NOPIN: their payload is copied and their leaf
 * let go.  The count copes with those without weakening §9 - see
 * lion_count_sources_run(), which either locates such a set again under a pin
 * of its own when it gets to it, or counts it in an intersection another
 * source carries the interlock for, or trusts no visibility map at all.
 */
int
lion_posting_set_lookup_many_col(Relation index, AttrNumber attno, Oid keytype,
								int nvalues, const Datum *values,
								const bool *isnull, LionPostingSet *sets,
								int *nfound)
{
	LionState   *state = lion_index_column_state(index, attno);
	LionProbe	probe;
	LionProbeSort sortctx;
	LionProbeKey *probes;
	const Datum *vals;
	Buffer		buf = InvalidBuffer;
	Buffer		lastpinned = InvalidBuffer;
	bool		lastmoved = false;
	uint32		budget;
	uint32		npinned = 0;
	int			nprobe = 0;
	int			nsets = 0;
	int			found = 0;
	int			runstart = 0;	/* first set located under this sort run */
	int			i;
	int			j;

	Assert(nvalues >= 0);
	if (nfound != NULL)
		*nfound = 0;
	if (nvalues == 0)
		return 0;

	/*
	 * One resolution for both lookups (DESIGN.md §21).  Whatever
	 * lion_probe_init() decides - the family's cross-type ordering, a cast to
	 * the key type, or no ordering at all - applies here exactly as it does to
	 * lion_posting_set_lookup(), because the two walk the same tree and a
	 * batched lookup that descended where the single one scans would read the
	 * directory in an order it is not in.
	 */
	lion_probe_init(index, state, keytype, &probe);

	budget = lion_list_pin_budget(index);

	/* A binary coercion - the only one taken - changes no value. */
	vals = values;

	/* an array parameter has no length cap (DESIGN.md §15) */
	probes = (LionProbeKey *) palloc_extended(sizeof(LionProbeKey) * nvalues,
											  MCXT_ALLOC_HUGE);
	for (i = 0; i < nvalues; i++)
	{
		if (isnull != NULL && isnull[i])
			continue;			/* `col = NULL` is never true */
		probes[nprobe].hash = lion_probe_hash(state, &probe, vals[i]);
		probes[nprobe].idx = i;
		nprobe++;
		if ((nprobe & 0x3ff) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	sortctx.values = vals;
	sortctx.probe = &probe;
	sortctx.collation = state->collation;
	sortctx.ncmp = 0;
	if (nprobe > 1)
		qsort_arg(probes, nprobe, sizeof(LionProbeKey), lion_probe_sort_cmp,
				  &sortctx);

	for (i = 0; i < nprobe; i++)
	{
		LionSearchKey sk;
		OffsetNumber off;
		bool		dup = false;
		bool		located;
		int			steps;
		LionRightWalk walk;

		/*
		 * Every so often, with no lock held.  The walk keeps the leaf it
		 * stands on LOCKED from one value to the next, and a content lock
		 * holds interrupts off, so the check this loop used to make answered
		 * no cancel however long the list (2026-09-28 review).  The leaf is
		 * let go here, and the next value descends again - what it does
		 * anyway whenever it lies more than a few leaves further right.  The
		 * pins the sets took are theirs and stay.
		 */
		if (i > 0 && (i & 0x3f) == 0)
		{
			if (BufferIsValid(buf))
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}
			CHECK_FOR_INTERRUPTS();
		}

		/*
		 * A new sort run starts a new set of possible duplicates: with an
		 * ordering that is the run of equal values, without one the run of
		 * equal hashes.
		 */
		if (i > 0 &&
			(probe.hassort ?
			 (lion_probe_key_cmp(&probes[i - 1], &probes[i], &sortctx) != 0 &&
			  probes[i].hash != probes[i - 1].hash) :
			 probes[i].hash != probes[i - 1].hash))
			runstart = nsets;

		/* The cheap half: bytewise-equal neighbours need no lookup at all. */
		if (i > 0 &&
			datumIsEqual(vals[probes[i].idx], vals[probes[i - 1].idx],
						 probe.typbyval, probe.typlen))
			continue;

		lion_probe_search_key(state, &probe, vals[probes[i].idx],
							 probes[i].hash, &sk);

		memset(&sets[nsets], 0, sizeof(LionPostingSet));
		sets[nsets].index = index;
		sets[nsets].attno = state->attno;
		sets[nsets].pinbuf = InvalidBuffer;
		sets[nsets].head = InvalidBlockNumber;
		sets[nsets].entryblk = InvalidBlockNumber;
		sets[nsets].entryoff = InvalidOffsetNumber;

		if (!probe.walk)
		{
			/*
			 * The values are not sorted in the directory's order (no ordering
			 * for them at all, or a cross-type one that cannot sort a list):
			 * there is no walk to keep, and every value is located exactly as
			 * the single lookup locates it - a descent, or the leaf walk.
			 */
			if (BufferIsValid(buf))
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}
			located = lion_dir_find(index, NULL, state->ix, &sk,
									BUFFER_LOCK_SHARE, false, &buf, &off,
									NULL);
		}
		else
		{
			/*
			 * Stay on the leaf the last value was found on when the next one is
			 * at most a few pages to the right; otherwise descend again.
			 *
			 * Not when the last lookup followed its prefix run across a page
			 * boundary, though: the walk then stands to the RIGHT of where
			 * that run begins, and the next value may belong to the same run
			 * (a hash collision) and be stored on one of the pages it has
			 * passed.  The leaf a lookup lands on otherwise is the one its
			 * run begins on, so a value sorted after it can only be there or
			 * further right.
			 */
			if (lastmoved && BufferIsValid(buf))
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}

			lion_rightwalk_init(&walk);
			for (steps = 0; BufferIsValid(buf); steps++)
			{
				Page		page = BufferGetPage(buf);

				if (LionPageIsRightmost(page) ||
					lion_cmp_entry(lion_dir_highkey(page), &sk) > 0)
					break;
				if (steps >= LION_LOOKUP_WALK_MAX)
				{
					UnlockReleaseBuffer(buf);
					buf = InvalidBuffer;
					break;
				}
				buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE,
										  &walk);
			}

			if (!BufferIsValid(buf))
				buf = lion_dir_search(index, NULL, state->ix, &sk,
									  BUFFER_LOCK_SHARE, false, &off);
			else
				off = lion_dir_binsrch(BufferGetPage(buf), &sk);

			located = lion_dir_scan_run(index, &sk, BUFFER_LOCK_SHARE,
										&buf, &off, &lastmoved);
		}

		if (located)
		{
			bool		keeppin;

			lion_fill_posting_set(index, state, buf, off, &sets[nsets],
								  &keeppin);
			if (keeppin)
			{
				/*
				 * DESIGN.md §9: the INLINE payload just copied out needs a pin
				 * of its own on this leaf, independent of the walk's position
				 * - if the leaf is one of the first `budget`.  Another pin on
				 * the leaf the last set pinned costs no buffer; a new leaf is
				 * counted.  Without a walk the leaves come in hash order and
				 * may repeat, which counts some twice: the budget can only be
				 * reached early, never overrun.
				 */
				if (buf != lastpinned && npinned >= budget)
					sets[nsets].nopin = true;
				else
				{
					if (buf != lastpinned)
					{
						npinned++;
						lion_list_pin_charge(&sets[nsets]);
					}
					lastpinned = buf;
					IncrBufferRefCount(buf);
					sets[nsets].pinbuf = buf;
				}
			}

			/*
			 * Two values that are not bytewise equal may still be the same
			 * entry (citext).  The stored keys are of the index's own type, so
			 * its own equality settles it exactly.
			 */
			for (j = runstart; j < nsets; j++)
			{
				if (sets[j].found && sets[j].hasstoredkey &&
					sets[nsets].hasstoredkey &&
					sets[j].keyisnull == sets[nsets].keyisnull &&
					(sets[nsets].keyisnull ||
					 lion_keys_equal(state, sets[j].storedkey,
									 sets[nsets].storedkey)))
				{
					/*
					 * If it took a new leaf, forget the leaf too: with its
					 * pin gone the buffer may be another page by the time the
					 * walk lands on it again.  Sets before it may still pin
					 * the leaf, and the next set there counts it once more -
					 * early, never over.
					 */
					if (sets[nsets].budgeted)
						lastpinned = InvalidBuffer;
					lion_posting_set_release(&sets[nsets]);
					dup = true;
					break;
				}
			}
			if (dup)
				continue;
			found++;
		}

		nsets++;
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	pfree(probes);
	if (nfound != NULL)
		*nfound = found;
	return nsets;
}

/* ---------------------------------------------------------------------
 * A walk of the leaves for keys that come one at a time (DESIGN.md §27)
 * --------------------------------------------------------------------- */

/*
 * Start a walk of key column `attno` of index for keys of type keytype
 * (InvalidOid: the column's own).  The resolution is lion_probe_init()'s, made
 * once for every key instead of once per lookup, and what the walk keeps -
 * the probe's FmgrInfos and the copy of a high key - is allocated in the
 * current memory context, which must outlive the walk.  The column state it
 * keeps is valid for as long as the caller keeps the index open, a relcache
 * flush included (LionAmCache, lion_state.c).  Nothing is read yet.
 */
void
lion_lookup_walk_begin(LionLookupWalk *walk, Relation index, AttrNumber attno,
					   Oid keytype)
{
	memset(walk, 0, sizeof(LionLookupWalk));
	walk->index = index;
	walk->state = lion_index_column_state(index, attno);
	lion_probe_init(index, walk->state, keytype, &walk->probe);
	walk->blk = InvalidBlockNumber;
	walk->buf = InvalidBuffer;
	walk->rightlink = InvalidBlockNumber;
	walk->prefetched = InvalidBlockNumber;

	/* a high key is a pivot: a header and a key, never a payload */
	walk->hikey = (LionEntryTuple *)
		palloc(MAXALIGN(LION_ENTRY_HDRSZ + LION_MAX_KEY_SIZE));
	walk->hikeylen = 0;

	/*
	 * Stepping right over s leaves reads s pages where a descent reads the
	 * root, the levels below it and the leaf: height + 1.  So a walk steps
	 * over at most `height` of them and descends for a key further away.
	 */
	walk->maxsteps = (int) Max(walk->state->ix->meta.height, 1);
	walk->stepok = true;

	/*
	 * The leaves come in the order the keys do, so the next one can be asked
	 * for before it is needed - where the storage can overlap the reads at
	 * all, which is what the tablespace's io concurrency says.
	 */
	walk->prefetch =
		(get_tablespace_io_concurrency(index->rd_rel->reltablespace) > 0);
}

/*
 * Can the keys be sorted into the directory's order (lion_lookup_walk_cmp())?
 * Without that each key is located by a descent of its own, in any order.
 */
bool
lion_lookup_walk_ordered(const LionLookupWalk *walk)
{
	return walk->probe.walk;
}

/* The hash a key is sorted by, after its comparison, and searched with. */
uint32
lion_lookup_walk_hash(LionLookupWalk *walk, Datum key)
{
	return lion_probe_hash(walk->state, &walk->probe,
						   lion_probe_value(&walk->probe, key));
}

/*
 * The directory's order of two keys (lion_probe_key_cmp()): the probe's own
 * comparison of two values of the key type, then the hash, which is all an
 * unordered column's directory is ordered by.  Keys that tie are the same
 * position in the directory - the same entry, or a run of hash collisions
 * that a lookup of either scans from its start.  A long sort calls this some
 * n log n times through the opclass, and nothing else in it answers a cancel;
 * the caller holds no lock while it sorts.
 */
int
lion_lookup_walk_cmp(LionLookupWalk *walk, Datum a, uint32 ahash, Datum b,
					 uint32 bhash)
{
	if ((++walk->ncmp & 0xffff) == 0)
		CHECK_FOR_INTERRUPTS();
	if (walk->probe.hassort)
	{
		int32		c = DatumGetInt32(FunctionCall2Coll(&walk->probe.sortproc,
														walk->state->collation,
														a, b));

		if (c != 0)
			return c < 0 ? -1 : 1;
	}
	if (ahash != bhash)
		return ahash < bhash ? -1 : 1;
	return 0;
}

/* The order of lion_lookup_walk_cmp(), as lion_walk_order_equal() compares it. */
void
lion_lookup_walk_order(const LionLookupWalk *walk, LionWalkOrder *order)
{
	memset(order, 0, sizeof(LionWalkOrder));
	order->valid = walk->probe.walk;
	order->hassort = walk->probe.hassort;
	if (walk->probe.hassort)
		order->sortproc = walk->probe.sortproc.fn_oid;
	order->hashproc = walk->probe.crosstype ? walk->probe.hashinfo.fn_oid :
		walk->state->hashproc.fn_oid;
	order->collation = walk->state->collation;
}

bool
lion_walk_order_equal(const LionWalkOrder *a, const LionWalkOrder *b)
{
	return a->valid && b->valid && a->hassort == b->hassort &&
		a->sortproc == b->sortproc && a->hashproc == b->hashproc &&
		a->collation == b->collation;
}

/* Let go of the leaf the walk stands on, and remember where it was. */
void
lion_lookup_walk_pause(LionLookupWalk *walk)
{
	if (BufferIsValid(walk->buf))
		ReleaseBuffer(walk->buf);
	walk->buf = InvalidBuffer;
}

/* ... and forget it too: the next key may be anywhere. */
void
lion_lookup_walk_restart(LionLookupWalk *walk)
{
	lion_lookup_walk_pause(walk);
	walk->blk = InvalidBlockNumber;
	walk->rightlink = InvalidBlockNumber;
	walk->hikeylen = 0;
	walk->stepok = true;
}

/*
 * Locate the posting set of key, whose hash is lion_lookup_walk_hash()'s,
 * into *ps - exactly the set lion_posting_set_lookup_col() would locate, and
 * as a single lookup does, an INLINE set keeps a pin of its own on its leaf
 * (DESIGN.md §9).  The key must not sort before the last one this walk
 * located since it was begun or restarted.
 *
 * Where the walk starts looking:
 *
 *	- below the high key the last leaf had: that leaf, pinned still or read
 *	  again by its block number.  If it has split since, the key may have gone
 *	  to a page on its right, and the walk moves right as a descent does;
 *	- at or above it, while the keys have been close: the leaf that was its
 *	  right sibling.  That page's lower bound was the high key just passed, and
 *	  a lower bound never moves, so the key is there or further right - even if
 *	  the last leaf has split and has a new right sibling in between, which then
 *	  holds only keys below that bound.  At most maxsteps pages further right;
 *	- otherwise, or past those: a descent from the root.  A descent that lands
 *	  on the leaf right of the last one says the keys are close again.
 *
 * A key found by following a run of hash collisions across a page boundary
 * leaves the walk to the right of where that run begins, and the next key may
 * belong to it: the walk forgets its place then, as the list walk does
 * (lion_posting_set_lookup_many_col()).
 *
 * No lock is held on return, and a pin only on the leaf the key was located
 * on, when the set is INLINE and pins it as well, or when the key has no
 * entry and no count follows; lion_lookup_walk_pause() lets it go.  A CHAIN
 * set's count reads the posting tree for as long as it takes, and the leaf is
 * not held through it.
 */
bool
lion_lookup_walk_find(LionLookupWalk *walk, Datum key, uint32 hash,
					  LionPostingSet *ps)
{
	Relation	index = walk->index;
	LionSearchKey sk;
	Buffer		buf = InvalidBuffer;
	OffsetNumber off;
	BlockNumber prevright = walk->rightlink;
	bool		had = BlockNumberIsValid(walk->blk);
	bool		stepping = false;
	bool		located;
	bool		moved = false;
	bool		keeppin = false;
	int			steps;
	LionRightWalk rwalk;

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = walk->state->attno;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;
	ps->entryblk = InvalidBlockNumber;
	ps->entryoff = InvalidOffsetNumber;

	key = lion_probe_value(&walk->probe, key);
	lion_probe_search_key(walk->state, &walk->probe, key, hash, &sk);

	if (!walk->probe.walk)
	{
		/*
		 * The keys are in no order the directory knows: every one is located
		 * as the single lookup locates it - a descent, or the leaf walk.
		 */
		if (!lion_dir_find(index, NULL, walk->state->ix, &sk,
						   BUFFER_LOCK_SHARE, false, &buf, &off, NULL))
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			return false;
		}
		lion_posting_set_take(index, walk->state, buf, off, ps);
		return true;
	}

	if (had)
	{
		lion_rightwalk_init(&rwalk);
		if (walk->hikeylen == 0 || lion_cmp_entry(walk->hikey, &sk) > 0)
		{
			/* on the last leaf, or right of it if it has split since */
			if (BufferIsValid(walk->buf))
			{
				buf = walk->buf;
				walk->buf = InvalidBuffer;
				LockBuffer(buf, BUFFER_LOCK_SHARE);
			}
			else
				buf = lion_dir_read_leaf(index, walk->blk);
			walk->stepok = true;
		}
		else
		{
			lion_lookup_walk_pause(walk);
			if (walk->stepok && BlockNumberIsValid(walk->rightlink))
			{
				lion_rightwalk_step(index, &rwalk, walk->blk, walk->rightlink);
				buf = lion_dir_read_leaf(index, walk->rightlink);
				stepping = true;
			}
		}

		/*
		 * Right, past what split off since - as far as it takes, as in a
		 * descent - or past leaves with no key of the caller's, a few at most.
		 */
		for (steps = 1; BufferIsValid(buf); steps++)
		{
			Page		page = BufferGetPage(buf);

			if (LionPageIsRightmost(page) ||
				lion_cmp_entry(lion_dir_highkey(page), &sk) > 0)
				break;
			if (stepping && steps >= walk->maxsteps)
			{
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
				walk->stepok = false;
				break;
			}
			buf = lion_dir_step_right(index, buf, BUFFER_LOCK_SHARE, &rwalk);
		}
	}
	else
		lion_lookup_walk_pause(walk);

	if (BufferIsValid(buf))
		off = lion_dir_binsrch(BufferGetPage(buf), &sk);
	else
	{
		buf = lion_dir_search(index, NULL, walk->state->ix, &sk,
							  BUFFER_LOCK_SHARE, false, &off);
		if (had)
			walk->stepok = (BufferGetBlockNumber(buf) == prevright);
	}

	/*
	 * Where the walk stands now: the leaf the key's run begins on, its right
	 * sibling and its high key, copied while it is locked.  A high key longer
	 * than a pivot can be is damage the next read of the page reports; the
	 * walk just does not keep its place past it.
	 */
	{
		Page		page = BufferGetPage(buf);

		walk->blk = BufferGetBlockNumber(buf);
		walk->rightlink = LionPageGetOpaque(page)->rightlink;
		walk->hikeylen = 0;
		if (!LionPageIsRightmost(page))
		{
			ItemId		iid = PageGetItemId(page, FirstOffsetNumber);
			LionEntryTuple *hk = (LionEntryTuple *) PageGetItem(page, iid);
			Size		len = ItemIdGetLength(iid);

			if (len >= LION_ENTRY_HDRSZ &&
				len <= MAXALIGN(LION_ENTRY_HDRSZ + LION_MAX_KEY_SIZE) &&
				(Size) hk->keylen <= len - LION_ENTRY_HDRSZ)
			{
				memcpy(walk->hikey, hk, len);
				walk->hikeylen = len;
			}
			else
				walk->blk = InvalidBlockNumber;
		}
	}

	located = lion_dir_scan_run(index, &sk, BUFFER_LOCK_SHARE, &buf, &off,
								&moved);
	if (located)
	{
		lion_fill_posting_set(index, walk->state, buf, off, ps, &keeppin);
		if (keeppin)
		{
			IncrBufferRefCount(buf);
			ps->pinbuf = buf;
		}
	}

	if (moved)
	{
		UnlockReleaseBuffer(buf);
		lion_lookup_walk_restart(walk);
		return located;
	}

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	if (located && !keeppin)
		ReleaseBuffer(buf);
	else
		walk->buf = buf;

	/*
	 * Keys that come close together will want the next leaf too: ask for it
	 * now, with nothing locked, so that the read overlaps the count.
	 */
	if (walk->prefetch && walk->stepok && BlockNumberIsValid(walk->blk) &&
		BlockNumberIsValid(walk->rightlink) &&
		walk->rightlink != walk->prefetched)
	{
		(void) PrefetchBuffer(index, MAIN_FORKNUM, walk->rightlink);
		walk->prefetched = walk->rightlink;
	}

	return located;
}

/*
 * One key's set by a descent of its own, as lion_posting_set_lookup_col()
 * locates it, but with the probe lion_lookup_walk_begin() resolved: for a key
 * of another type than the column's, lion_probe_init() looks up the family's
 * cross-type operators and support functions in the catalogs and sets up
 * their FmgrInfos, which a lookup per key of an FK-side join made thousands
 * of times over for the same answer.  The key needs no order and the walk
 * keeps its place: the leaf found is released here, as a single lookup's is.
 */
bool
lion_lookup_walk_descend(LionLookupWalk *walk, Datum key, LionPostingSet *ps)
{
	uint32		hash;

	key = lion_probe_value(&walk->probe, key);
	hash = lion_probe_hash(walk->state, &walk->probe, key);
	return lion_posting_set_locate(walk->index, walk->state, &walk->probe, key,
								   hash, ps);
}

/*
 * The same for the rows whose key is NULL (DESIGN.md §14).  The entry sorts
 * before every real key (LION_KIND_NULL), so the descent finds it on the
 * leftmost leaf; everything after that - the pin discipline of DESIGN.md §9
 * included - is identical to a real key's.
 */
bool
lion_posting_set_lookup_null_col(Relation index, AttrNumber attno,
								LionPostingSet *ps)
{
	LionState   *state = lion_index_column_state(index, attno);
	Buffer		buf;
	OffsetNumber off;

	memset(ps, 0, sizeof(LionPostingSet));
	ps->index = index;
	ps->attno = state->attno;
	ps->pinbuf = InvalidBuffer;
	ps->head = InvalidBlockNumber;
	ps->entryblk = InvalidBlockNumber;
	ps->entryoff = InvalidOffsetNumber;

	if (!lion_find_null_entry(index, state, BUFFER_LOCK_SHARE, &buf, &off))
	{
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
		return false;
	}

	lion_posting_set_take(index, state, buf, off, ps);
	return true;
}

void
lion_posting_set_unpin(LionPostingSet *ps)
{
	if (BufferIsValid(ps->pinbuf))
	{
		ReleaseBuffer(ps->pinbuf);
		ps->pinbuf = InvalidBuffer;
		ps->nopin = true;
	}
	lion_list_pin_return(ps);
}

/*
 * Locate the entry of a NOPIN set again, this time keeping the pin (a single
 * lookup always does): what lion_count_one_set() counts a NOPIN set from.  The
 * stored key is of the index's own type, so this is a plain lookup of it -
 * the same entry, since a column has one entry per key, as it is NOW, which
 * is all a count that reads the index after locating the entry ever gets (a
 * chain is read page by page as the count goes, too).  An entry that has gone
 * meanwhile held nothing visible to anyone, because VACUUM deletes an entry
 * only once its set is empty; false then, with *fresh not found.
 */
bool
lion_posting_set_relocate(const LionPostingSet *ps, LionPostingSet *fresh)
{
	LionState   *state = lion_index_column_state(ps->index, ps->attno);
	LionProbe	probe;
	Datum		key;

	Assert(ps->found && ps->nopin && ps->hasstoredkey);

	if (ps->keyisnull)
	{
		Buffer		buf;
		OffsetNumber off;

		memset(fresh, 0, sizeof(LionPostingSet));
		fresh->index = ps->index;
		fresh->attno = ps->attno;
		fresh->pinbuf = InvalidBuffer;
		fresh->head = InvalidBlockNumber;
		if (!lion_find_null_entry(ps->index, state, BUFFER_LOCK_SHARE, &buf,
								  &off))
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			return false;
		}
		lion_posting_set_take(ps->index, state, buf, off, fresh);
		return true;
	}

	lion_probe_init(ps->index, state, InvalidOid, &probe);
	key = lion_probe_value(&probe, ps->storedkey);

	return lion_posting_set_locate(ps->index, state, &probe, key,
								   lion_probe_hash(state, &probe, key), fresh);
}

void
lion_posting_set_release(LionPostingSet *ps)
{
	if (BufferIsValid(ps->pinbuf))
		ReleaseBuffer(ps->pinbuf);
	lion_list_pin_return(ps);
	ps->pinbuf = InvalidBuffer;
	ps->nopin = false;
	ps->payload = NULL;			/* the memory belongs to the caller's context */
	ps->paylen = 0;

	/* a spilled copy owns its temporary file (lion_spill_finish()) */
	if (ps->mat != NULL && ps->mat->file != NULL)
	{
		BufFileClose(ps->mat->file);
		ps->mat->file = NULL;
	}

	/*
	 * ... and a view of a shared copy the files it opened of its chunks
	 * (lion_copy_view()); the files are the plan's, and stay.
	 */
	if (ps->mat != NULL && ps->mat->files != NULL)
	{
		int			c;

		for (c = 0; c < ps->mat->nchunks; c++)
		{
			if (ps->mat->files[c] != NULL)
				BufFileClose(ps->mat->files[c]);
			ps->mat->files[c] = NULL;
		}
		ps->mat->files = NULL;
	}
	ps->mat = NULL;				/* ... and so does the materialized copy */
	ps->matfailed = false;
	ps->nuses = 0;
	ps->hasstoredkey = false;
	ps->keyisnull = false;
	ps->found = false;
}
