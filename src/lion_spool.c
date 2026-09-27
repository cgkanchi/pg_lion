/*-------------------------------------------------------------------------
 *
 * lion_spool.c
 *		The build's input (DESIGN.md §24, "Build"): each key column's posting
 *		sets gathered from the heap scan, spilled in sorted runs when memory
 *		runs out, and merged back out in directory order.
 *
 * A heap scan delivers TIDs in ascending order, so a posting set needs no
 * sorting at all.  Each key's codes are APPENDED as they arrive, to an
 * in-memory entry found through a hash table of the column's keys (the
 * opclass hash and equality, which the directory uses anyway), and only the
 * DISTINCT keys are ever sorted: K log K comparisons for a column of K keys,
 * where the tuplesort this replaces made N log N of them over every one of
 * the N (key, code) pairs - and, since equal keys tie on the key, fell
 * through to comparing the codes almost every time.  GIN's build has the same
 * shape (ginBuildCallback() and its BuildAccumulator).
 *
 * An entry keeps its codes as varbyte-coded DELTAS (lion_varbyte_put()), the
 * first counted from zero, which for the dense keys Lion is built for is one
 * or two bytes a row; a key with a single code - most keys of a
 * high-cardinality column - keeps it in the entry and allocates nothing.
 *
 * "Ascending" has two exceptions, and both are handled here rather than
 * assumed away:
 *
 *	- A heap-only tuple is reported under its HOT chain's ROOT offset
 *	  (heapam_index_build_range_scan()), while the scan walks the page in
 *	  physical order, so the codes of one heap page can arrive out of order.
 *	  Only within that page: the scan finishes a page before it starts the
 *	  next.  A code below the one before it is written as a zero delta (no
 *	  real delta is zero) followed by the code itself, the entry is marked
 *	  unsorted, and whoever reads it back re-sorts it one heap page at a time
 *	  (lion_cursor_step()), which is all the disorder there can be.
 *
 *	- A synchronized scan may start in the middle of the table and wrap
 *	  around.  The builds ask for none (lion_build.c), but should the block
 *	  number ever go DOWN, every column is spilled at that point, so that no
 *	  run holds codes from both sides of the jump; the merge below orders
 *	  codes across runs whatever their order, so nothing else changes.
 *
 * MEMORY is one budget for all the key columns together, not an even share
 * each (which the tuplesorts needed): a column takes what its keys cost, so a
 * boolean column takes a few bytes a row and a column of unique keys takes
 * the most.  It is checked when the scan moves to the next heap page, and
 * when it is exceeded the LARGEST columns are spilled until half of it is
 * free.  Spilling a column sorts its entries into directory order and writes
 * them to a logical tape as one RUN, after which its memory is reset; runs
 * only ever start and end at a page boundary, so no page's codes are split
 * between two of them.
 *
 * The MERGE takes a column's runs and what is still in memory, k-way by key
 * (lion_merge_column()).  The inputs holding equal keys are merged by CODE,
 * which for the runs of one serial scan means taking them one after the other
 * and for the participants of a parallel build means interleaving them one
 * block chunk at a time.  The key an entry is written with is the one its
 * smallest code came with, which matters for an opclass whose equality is
 * coarser than the bytes ('Alice' and 'alice' under citext, 1.0 and 1.00 as
 * numeric) and is what the sorted build wrote: its sort put the smallest code
 * of a key first.
 *
 * A PARALLEL build (lion_build.c) gives each participant a spool of its own
 * and a shared fileset; at the end of its part of the scan a participant
 * merges its runs into ONE tape with a section per column
 * (lion_spool_export()), and the leader merges the participants' tapes a
 * column at a time exactly as a serial build merges its runs
 * (lion_spool_reader_emit_column()).  The entries and the codes that come out
 * are the same either way, so the index is too, page for page.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "utils/logtape.h"
#include "utils/memutils.h"
#include "utils/sortsupport.h"
#include "varatt.h"

#include "lion.h"
#include "lion_spool.h"

/* A varbyte-coded uint64 takes at most this many bytes. */
#define LION_VARBYTE_MAX	10

/* A code a stream holds out of order: a zero delta, then the code. */
#define LION_ESCAPED_MAX	(1 + LION_VARBYTE_MAX)

/* A run's codes are written in chunks of at most this many bytes. */
#define LION_RUN_CHUNK		8192

/* Entries and their keys are carved out of blocks of at most this size. */
#define LION_ARENA_BLOCK	((Size) 64 * 1024)

/* The most codes one key can have on one heap page. */
#define LION_PAGE_CODES		(1 << LION_OFFSET_BITS)

/*
 * A record's flags.  TIED: the next record of the same run has the same
 * PREFIX (lion_merge_prefix_cmp()) - the same hash under an unordered
 * opclass - so the merge has to take both at once.
 */
#define LION_RUN_TIED		0x01

/* Where a record's kind would be: the end of one column's records. */
#define LION_RUN_END		0

/*
 * One key's codes, as the scan delivered them (see the file header).  key is
 * the value itself for a by-value type and points at the key's stored bytes
 * (lion_store_key()) otherwise.
 */
typedef struct LionAccEntry
{
	Datum		key;
	uint32		hash;
	uint8		kind;			/* LION_KIND_NULL, _EMPTY or _VALUE */
	bool		unsorted;		/* a code arrived below the one before it */
	Size		keylen;			/* lion_key_datum_size(), 0 when reserved */
	uint64		mincode;
	uint64		lastcode;
	uint8	   *buf;			/* every code, from the second one on */
	Size		len;
	Size		cap;
} LionAccEntry;

typedef struct LionAccSlot
{
	LionAccEntry *entry;
	uint32		hash;			/* murmurhash32() of entry->hash */
	char		status;
} LionAccSlot;

struct LionAccCol;
static inline bool lion_acc_equal(struct LionAccCol *col,
								  const LionAccEntry *a,
								  const LionAccEntry *b);

/*
 * The opclass hash goes through murmurhash32() before it picks a bucket: the
 * table takes its low bits, and nothing says an opclass's hash spreads them.
 */
#define SH_PREFIX		lionacc
#define SH_ELEMENT_TYPE	LionAccSlot
#define SH_KEY_TYPE		LionAccEntry *
#define SH_KEY			entry
#define SH_HASH_KEY(tb, key)	murmurhash32((key)->hash)
#define SH_EQUAL(tb, a, b)	lion_acc_equal((struct LionAccCol *) (tb)->private_data, a, b)
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a)	((a)->hash)
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/* One key column's accumulator, and the runs it has spilled. */
typedef struct LionAccCol
{
	LionState  *state;
	MemoryContext cxt;			/* the table, the entries, keys and codes */
	lionacc_hash *table;		/* the VALUE entries */
	LionAccEntry *reserved[2];	/* the NULL and the EMPTY entry, if any */
	char	   *arena;
	Size		arenafree;
	Size		arenablock;		/* the size of an arena block */
	Size		emptysize;		/* what cxt holds with nothing in it */
	int64		nentries;		/* in memory, the reserved ones included */
	LogicalTape **runs;			/* in the spool's tape set */
	int			nruns;
	int			maxruns;
} LionAccCol;

struct LionSpool
{
	LionIndexState *ix;
	int			ncols;
	LionAccCol *cols;
	MemoryContext cxt;			/* everything the spool has */
	MemoryContext rowcxt;		/* reset after every heap row */
	MemoryContext mergecxt;		/* reset after every merge */
	Size		membytes;		/* what the accumulators may use together */
	Size		readbuf;		/* the read buffer of one run in a merge */
	int			maxorder;		/* the most runs one merge reads at once */
	SharedFileSet *fileset;		/* a parallel participant's, or NULL */
	int			filenum;
	LogicalTapeSet *tapes;		/* created at the first spill */
	BlockNumber curblk;			/* the heap page the scan is on */
	bool		haveblk;
	double		ntids;			/* (key, code) pairs added */
	int			nspills;		/* runs spilled, all columns together */
};

struct LionSpoolReader
{
	LionIndexState *ix;
	MemoryContext cxt;
	MemoryContext mergecxt;
	LogicalTapeSet *tapes;
	int			nparts;
	struct LionRunCursor **curs;
};

/* A record's header on a tape, followed by its key and its code chunks. */
typedef struct LionRunHeader
{
	uint8		kind;
	uint8		flags;
	uint16		unused;
	uint32		hash;
	uint32		keylen;
} LionRunHeader;

typedef struct LionRunWriter
{
	LogicalTape *tape;
	uint64		last;			/* the code the next delta counts from */
	Size		len;
	uint8		buf[LION_RUN_CHUNK];
} LionRunWriter;

/*
 * A cursor over one input of a merge: a run on a tape, or a column's entries
 * still in memory (in directory order, from lion_acc_sorted()).  It is always
 * on a record - one entry - or at the end of the column (eos), and within the
 * record on its next code (hascode/code) in ascending order.
 */
typedef struct LionRunCursor
{
	LionState  *state;
	int			index;			/* among the merge's inputs */
	MemoryContext cxt;			/* what the cursor allocates lives here */

	LogicalTape *tape;
	struct LionSortItem *items; /* ... or these, in memory */
	uint8	   *itemflags;
	int			nitems;
	int			nextitem;

	/* the current record */
	bool		eos;
	uint8		kind;
	uint8		flags;
	uint32		hash;
	Datum		key;
	const char *raw;
	Size		rawlen;
	char	   *keybuf;			/* a tape record's key bytes */
	Size		keycap;
	uint64		firstcode;

	/* its codes, and the decoder's state */
	bool		hascode;
	uint64		code;
	uint64		last;
	const uint8 *p;
	const uint8 *end;
	bool		lastchunk;		/* nothing of the record beyond p..end */
	uint8	   *cbuf;			/* a tape record's current chunk */
	uint8		single[LION_VARBYTE_MAX];

	/* an unsorted stream is handed out one heap page at a time */
	bool		resort;
	uint64	   *page;
	int			npage;
	int			ipage;
	bool		havepeek;
	uint64		peek;
} LionRunCursor;

/* An entry in the order a spill or a merge wants it. */
typedef struct LionSortItem
{
	Datum		datum1;			/* the abbreviated key, or the key */
	LionAccEntry *entry;
} LionSortItem;

typedef struct LionSortCtx
{
	LionState  *state;
	SortSupport ssup;
	bool		abbreviated;
	bool		sawtie;			/* two keys compared equal */
} LionSortCtx;

/* The merge of one column's inputs. */
typedef struct LionMerge
{
	LionState  *state;
	SortSupportData ssup;		/* an ordered column's full comparison */
	MemoryContext cxt;			/* a materialised group, reset after it */
} LionMerge;

struct LionSpoolGroup
{
	int			nentries;
	LionSpoolEntry *entries;
	LionSpoolEntry one;

	/* one entry, whose codes these cursors hold between them */
	LionRunCursor **curs;
	int			ncurs;
	int			active;			/* the cursor being drained, or -1 */
	uint64		limit;			/* ... while its codes stay below this */
	bool		havelimit;

	/* a hash collision: every entry's codes, sorted */
	bool		materialised;
	uint64	  **codes;
	uint64	   *ncodes;
	uint64	   *pos;
};

static void lion_spool_spill_column(LionSpool *sp, LionAccCol *col);
static void lion_cursor_step(LionRunCursor *c);

/* ---------------------------------------------------------------------
 * Varbyte codes
 * --------------------------------------------------------------------- */

static inline int
lion_varbyte_put(uint8 *p, uint64 v)
{
	int			n = 0;

	while (v >= 0x80)
	{
		p[n++] = (uint8) (v | 0x80);
		v >>= 7;
	}
	p[n++] = (uint8) v;
	return n;
}

static inline uint64
lion_varbyte_get(const uint8 **pp, const uint8 *end)
{
	const uint8 *p = *pp;
	uint64		v = 0;
	int			shift = 0;
	uint8		b;

	do
	{
		if (p >= end || shift > 63)
			elog(ERROR, "lion index build: malformed code in a spilled run");
		b = *p++;
		v |= ((uint64) (b & 0x7F)) << shift;
		shift += 7;
	} while (b & 0x80);

	*pp = p;
	return v;
}

static int
lion_code_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/* ---------------------------------------------------------------------
 * The accumulator
 * --------------------------------------------------------------------- */

static inline const char *
lion_acc_raw(const LionState *cs, const LionAccEntry *e)
{
	if (e->kind != LION_KIND_VALUE)
		return NULL;
	return cs->typbyval ? (const char *) &e->key : DatumGetPointer(e->key);
}

static inline bool
lion_acc_equal(LionAccCol *col, const LionAccEntry *a, const LionAccEntry *b)
{
	return a->hash == b->hash && lion_keys_equal(col->state, a->key, b->key);
}

static void *
lion_acc_alloc(LionAccCol *col, Size size)
{
	void	   *p;

	size = MAXALIGN(size);
	if (size > col->arenafree)
	{
		Size		blk = Max(col->arenablock, size);

		col->arena = MemoryContextAlloc(col->cxt, blk);
		col->arenafree = blk;
	}
	p = col->arena;
	col->arena += size;
	col->arenafree -= size;
	return p;
}

/* Forget everything in memory: after a spill, or when the column is done. */
static void
lion_acc_reset(LionAccCol *col)
{
	MemoryContextReset(col->cxt);
	col->table = lionacc_create(col->cxt, 256, col);
	col->reserved[0] = col->reserved[1] = NULL;
	col->arena = NULL;
	col->arenafree = 0;
	col->nentries = 0;
	col->emptysize = MemoryContextMemAllocated(col->cxt, true);
}

/*
 * What a column's entries take.  The empty table and the context's first
 * block are not counted: they are there whatever maintenance_work_mem says,
 * and counting them would make a small budget on a wide index spill every
 * column at every heap page.
 */
static Size
lion_acc_used(LionAccCol *col)
{
	Size		size = MemoryContextMemAllocated(col->cxt, true);

	return (size > col->emptysize) ? size - col->emptysize : 0;
}

static LionAccEntry *
lion_acc_new_entry(LionAccCol *col, int kind, Datum key, uint32 hash,
				   uint64 code)
{
	LionState  *cs = col->state;
	LionAccEntry *e = lion_acc_alloc(col, sizeof(LionAccEntry));

	e->kind = (uint8) kind;
	e->hash = hash;
	e->unsorted = false;
	e->mincode = code;
	e->lastcode = code;
	e->buf = NULL;
	e->len = 0;
	e->cap = 0;
	e->key = (Datum) 0;
	e->keylen = 0;
	if (kind == LION_KIND_VALUE)
	{
		/* lion_key_datum_size() is also what refuses a key too long to store */
		e->keylen = lion_key_datum_size(cs, key);
		if (cs->typbyval)
			e->key = key;
		else
		{
			char	   *raw = lion_acc_alloc(col, e->keylen);

			lion_store_key(cs, key, raw);
			e->key = PointerGetDatum(raw);
		}
	}
	col->nentries++;
	return e;
}

/*
 * A code below every one the entry has seen, which only a HOT chain's root
 * offset can bring: the entry is written with the key its smallest code came
 * with (see the file header), so take this one's if its bytes differ.
 */
static void
lion_acc_rekey(LionAccCol *col, LionAccEntry *e, Datum key)
{
	LionState  *cs = col->state;
	Size		len;
	char	   *raw;

	if (cs->typbyval)
	{
		e->key = key;
		return;
	}
	len = lion_key_datum_size(cs, key);
	raw = lion_acc_alloc(col, len);
	lion_store_key(cs, key, raw);
	if (len != e->keylen || memcmp(raw, DatumGetPointer(e->key), len) != 0)
	{
		e->key = PointerGetDatum(raw);
		e->keylen = len;
	}
}

static void
lion_acc_grow(LionAccCol *col, LionAccEntry *e)
{
	Size		need = e->len + LION_ESCAPED_MAX;

	if (e->buf == NULL)
	{
		/* the second code: the first moves into the buffer */
		e->cap = 32;
		e->buf = MemoryContextAlloc(col->cxt, e->cap);
		e->len = lion_varbyte_put(e->buf, e->lastcode);
		return;
	}
	if (need <= e->cap)
		return;
	e->cap = Max(e->cap * 2, need);
	e->buf = repalloc_huge(e->buf, e->cap);
}

static inline void
lion_acc_append(LionAccCol *col, LionAccEntry *e, uint64 code)
{
	if (unlikely(e->buf == NULL || e->len + LION_ESCAPED_MAX > e->cap))
		lion_acc_grow(col, e);
	if (likely(code > e->lastcode))
		e->len += lion_varbyte_put(e->buf + e->len, code - e->lastcode);
	else
	{
		/* a heap-only tuple under its root offset (see the file header) */
		Assert(code != e->lastcode);
		e->buf[e->len++] = 0;
		e->len += lion_varbyte_put(e->buf + e->len, code);
		e->unsorted = true;
		if (code < e->mincode)
			e->mincode = code;
	}
	e->lastcode = code;
}

static void
lion_acc_add(LionSpool *sp, LionAccCol *col, Datum key, uint64 code)
{
	LionAccEntry probe;
	LionAccSlot *slot;
	LionAccEntry *e;
	bool		found;

	probe.key = key;
	probe.hash = lion_hash_key(col->state, key);
	slot = lionacc_insert_hash(col->table, &probe, murmurhash32(probe.hash),
							   &found);
	if (!found)
		slot->entry = lion_acc_new_entry(col, LION_KIND_VALUE, key,
										 probe.hash, code);
	else
	{
		e = slot->entry;
		if (unlikely(code < e->mincode))
			lion_acc_rekey(col, e, key);
		lion_acc_append(col, e, code);
	}
	sp->ntids += 1;
}

static void
lion_acc_add_reserved(LionSpool *sp, LionAccCol *col, int kind, uint64 code)
{
	int			i = (kind == LION_KIND_NULL) ? 0 : 1;

	if (col->reserved[i] == NULL)
		col->reserved[i] = lion_acc_new_entry(col, kind, (Datum) 0,
											  LION_NULLKEY_HASH, code);
	else
		lion_acc_append(col, col->reserved[i], code);
	sp->ntids += 1;
}

/* ---------------------------------------------------------------------
 * Directory order
 * --------------------------------------------------------------------- */

/*
 * The tail of the directory order (DESIGN.md §21): the hash, then the stored
 * bytes.  lion_builder_cmp() in lion_build.c is the same order.
 */
static int
lion_tail_cmp(uint32 hash1, const char *raw1, Size len1,
			  uint32 hash2, const char *raw2, Size len2)
{
	int			c;

	if (hash1 != hash2)
		return hash1 < hash2 ? -1 : 1;
	c = memcmp(raw1, raw2, Min(len1, len2));
	if (c != 0)
		return c < 0 ? -1 : 1;
	return (len1 > len2) - (len1 < len2);
}

static int
lion_sortitem_cmp(const void *a, const void *b, void *arg)
{
	LionSortCtx *s = (LionSortCtx *) arg;
	const LionSortItem *x = (const LionSortItem *) a;
	const LionSortItem *y = (const LionSortItem *) b;
	int			c;

	if (s->state->ordered)
	{
		c = ApplySortComparator(x->datum1, false, y->datum1, false, s->ssup);
		if (c != 0)
			return c;
		if (s->abbreviated)
		{
			c = ApplySortAbbrevFullComparator(x->entry->key, false,
											  y->entry->key, false, s->ssup);
			if (c != 0)
				return c;
		}
		s->sawtie = true;
	}
	return lion_tail_cmp(x->entry->hash, lion_acc_raw(s->state, x->entry),
						 x->entry->keylen,
						 y->entry->hash, lion_acc_raw(s->state, y->entry),
						 y->entry->keylen);
}

/*
 * A column's entries in directory order: the NULL entry, the EMPTY entry,
 * then the values.  *flagsp gets LION_RUN_TIED for every value whose
 * successor ties with it on the prefix, or NULL when none does.
 *
 * An ordered column sorts through the key type's SortSupport, abbreviated
 * where the type offers it, exactly as tuplesort would - and as the
 * tuplesort this replaces did, whose leading key was the key for that reason
 * (DESIGN.md §21, "Bulk build").  Abbreviation is given up the way tuplesort
 * gives it up, when the converter's own estimate says it does not pay.
 */
static LionSortItem *
lion_acc_sorted(LionAccCol *col, uint8 **flagsp, int *np)
{
	LionState  *cs = col->state;
	int64		nvals = col->table->members;
	int			nres = 0;
	LionSortItem *items;
	LionSortItem *vals;
	uint8	   *flags = NULL;
	lionacc_iterator it;
	LionAccSlot *slot;
	int64		n = 0;
	int			i;

	if (col->nentries > (int64) (MaxAllocHugeSize / sizeof(LionSortItem)) - 2)
		elog(ERROR, "lion index build: too many keys in one column");
	items = MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(LionSortItem) * (nvals + 2));
	for (i = 0; i < 2; i++)
	{
		if (col->reserved[i] != NULL)
		{
			items[nres].datum1 = (Datum) 0;
			items[nres++].entry = col->reserved[i];
		}
	}
	vals = items + nres;

	lionacc_start_iterate(col->table, &it);
	while ((slot = lionacc_iterate(col->table, &it)) != NULL)
	{
		vals[n].datum1 = slot->entry->key;
		vals[n++].entry = slot->entry;
	}
	Assert(n == nvals);

	if (nvals > 1)
	{
		LionSortCtx s;
		SortSupportData ssup;

		s.state = cs;
		s.ssup = &ssup;
		s.abbreviated = false;
		s.sawtie = false;
		if (cs->ordered)
		{
			memset(&ssup, 0, sizeof(ssup));
			ssup.ssup_cxt = CurrentMemoryContext;
			ssup.ssup_collation = cs->collation;
			ssup.ssup_nulls_first = false;
			ssup.abbreviate = true;
			PrepareSortSupportFromOrderingOp(cs->ltopr, &ssup);

			if (ssup.abbrev_converter != NULL)
			{
				int64		next = 10;

				s.abbreviated = true;
				for (i = 0; i < nvals; i++)
				{
					vals[i].datum1 = ssup.abbrev_converter(vals[i].entry->key,
														   &ssup);
					if (i + 1 == next)
					{
						next *= 2;
						if (ssup.abbrev_abort(i + 1, &ssup))
						{
							s.abbreviated = false;
							break;
						}
					}
				}
				if (!s.abbreviated)
				{
					ssup.comparator = ssup.abbrev_full_comparator;
					ssup.abbrev_converter = NULL;
					for (i = 0; i < nvals; i++)
						vals[i].datum1 = vals[i].entry->key;
				}
			}
		}

		qsort_arg(vals, nvals, sizeof(LionSortItem), lion_sortitem_cmp, &s);

		/*
		 * Ties.  Two keys of an unordered opclass tie on the prefix when their
		 * hashes do.  Two of an ordered one never should - equal under the
		 * comparison means equal under the equality, and so one entry - but
		 * the sort says whether they did: every pair that ends up adjacent
		 * was compared directly, so no tie it saw means no adjacent tie.
		 */
		if (!cs->ordered || s.sawtie)
		{
			for (i = 0; i + 1 < nvals; i++)
			{
				bool		tied;

				if (cs->ordered)
					tied = (s.abbreviated ?
							ApplySortAbbrevFullComparator(vals[i].entry->key, false,
														  vals[i + 1].entry->key, false,
														  &ssup) :
							ApplySortComparator(vals[i].entry->key, false,
												vals[i + 1].entry->key, false,
												&ssup)) == 0;
				else
					tied = vals[i].entry->hash == vals[i + 1].entry->hash;
				if (tied)
				{
					if (flags == NULL)
					{
						flags = MemoryContextAllocHuge(CurrentMemoryContext,
													   nvals + nres);
						memset(flags, 0, nvals + nres);
					}
					flags[nres + i] = LION_RUN_TIED;
				}
			}
		}
	}

	*flagsp = flags;
	*np = (int) (nvals + nres);
	return items;
}

/* ---------------------------------------------------------------------
 * Runs
 * --------------------------------------------------------------------- */

static void
lion_tape_read(LogicalTape *tape, void *ptr, size_t size)
{
	if (LogicalTapeRead(tape, ptr, size) != size)
		elog(ERROR, "lion index build: unexpected end of a spilled run");
}

static void
lion_run_begin_record(LionRunWriter *w, uint8 kind, uint8 flags, uint32 hash,
					  const char *raw, Size rawlen)
{
	LionRunHeader h;

	h.kind = kind;
	h.flags = flags;
	h.unused = 0;
	h.hash = hash;
	h.keylen = (uint32) rawlen;
	LogicalTapeWrite(w->tape, &h, sizeof(h));
	if (rawlen > 0)
		LogicalTapeWrite(w->tape, raw, rawlen);
	w->last = 0;
	w->len = 0;
}

static void
lion_run_flush_chunk(LionRunWriter *w)
{
	uint32		n = (uint32) w->len;

	if (n == 0)
		return;
	LogicalTapeWrite(w->tape, &n, sizeof(n));
	LogicalTapeWrite(w->tape, w->buf, w->len);
	w->len = 0;
}

static inline void
lion_run_put_code(LionRunWriter *w, uint64 code)
{
	Assert(code > w->last);
	if (w->len + LION_VARBYTE_MAX > LION_RUN_CHUNK)
		lion_run_flush_chunk(w);
	w->len += lion_varbyte_put(w->buf + w->len, code - w->last);
	w->last = code;
}

/*
 * A whole record's codes that are already in the run encoding: an in-memory
 * entry's buffer, sorted.  The reader does not mind a code split between two
 * chunks (lion_cursor_fill()).
 */
static void
lion_run_put_encoded(LionRunWriter *w, const uint8 *p, Size len)
{
	Assert(w->len == 0 && w->last == 0);
	while (len > 0)
	{
		uint32		n = (uint32) Min(len, LION_RUN_CHUNK);

		LogicalTapeWrite(w->tape, &n, sizeof(n));
		LogicalTapeWrite(w->tape, p, n);
		p += n;
		len -= n;
	}
}

static void
lion_run_end_record(LionRunWriter *w)
{
	uint32		zero = 0;

	lion_run_flush_chunk(w);
	LogicalTapeWrite(w->tape, &zero, sizeof(zero));
}

static void
lion_run_end_column(LogicalTape *tape)
{
	LionRunHeader h;

	memset(&h, 0, sizeof(h));
	h.kind = LION_RUN_END;
	LogicalTapeWrite(tape, &h, sizeof(h));
}

/* ---------------------------------------------------------------------
 * Cursors
 * --------------------------------------------------------------------- */

static void
lion_cursor_init(LionRunCursor *c, LionState *state, int index)
{
	memset(c, 0, sizeof(LionRunCursor));
	c->state = state;
	c->index = index;
	c->cxt = CurrentMemoryContext;
	c->eos = true;
	c->lastchunk = true;
}

static void
lion_cursor_init_tape(LionRunCursor *c, LionState *state, int index,
					  LogicalTape *tape)
{
	lion_cursor_init(c, state, index);
	c->tape = tape;
	c->cbuf = palloc(LION_RUN_CHUNK + LION_ESCAPED_MAX);
	c->p = c->end = c->cbuf;
}

static void
lion_cursor_init_memory(LionRunCursor *c, LionState *state, int index,
						LionSortItem *items, uint8 *flags, int nitems)
{
	lion_cursor_init(c, state, index);
	c->items = items;
	c->itemflags = flags;
	c->nitems = nitems;
	c->nextitem = 0;
}

/*
 * Keep at least one whole code, escaped or not, between p and end, reading
 * chunks for as long as the record has them.
 */
static void
lion_cursor_fill(LionRunCursor *c)
{
	while (!c->lastchunk && c->end - c->p < LION_ESCAPED_MAX)
	{
		Size		rest = c->end - c->p;
		uint32		n;

		memmove(c->cbuf, c->p, rest);
		lion_tape_read(c->tape, &n, sizeof(n));
		if (n == 0)
			c->lastchunk = true;
		else
		{
			if (n > LION_RUN_CHUNK)
				elog(ERROR, "lion index build: malformed chunk in a spilled run");
			lion_tape_read(c->tape, c->cbuf + rest, n);
		}
		c->p = c->cbuf;
		c->end = c->cbuf + rest + n;
	}
}

/* The next code in the order the stream holds them. */
static inline bool
lion_cursor_raw_next(LionRunCursor *c, uint64 *code)
{
	uint64		d;

	if (c->tape != NULL)
		lion_cursor_fill(c);
	if (c->p >= c->end)
		return false;
	d = lion_varbyte_get(&c->p, c->end);
	if (d == 0)
		*code = lion_varbyte_get(&c->p, c->end);
	else
		*code = c->last + d;
	c->last = *code;
	return true;
}

/*
 * Move to the record's next code in ascending order.  An unsorted stream is
 * out of order within one heap page at most (see the file header), so it is
 * read a page at a time and each page's codes are sorted.
 */
static void
lion_cursor_step(LionRunCursor *c)
{
	uint64		x;
	uint64		blk;

	if (!c->resort)
	{
		c->hascode = lion_cursor_raw_next(c, &c->code);
		return;
	}

	if (c->ipage < c->npage)
	{
		c->code = c->page[c->ipage++];
		c->hascode = true;
		return;
	}

	c->npage = 0;
	if (c->havepeek)
	{
		c->page[c->npage++] = c->peek;
		c->havepeek = false;
	}
	else if (lion_cursor_raw_next(c, &x))
		c->page[c->npage++] = x;
	else
	{
		c->hascode = false;
		return;
	}

	blk = c->page[0] >> LION_OFFSET_BITS;
	while (lion_cursor_raw_next(c, &x))
	{
		if ((x >> LION_OFFSET_BITS) != blk)
		{
			c->peek = x;
			c->havepeek = true;
			break;
		}
		if (c->npage >= LION_PAGE_CODES)
			elog(ERROR, "lion index build: more codes for one heap page than it has offsets");
		c->page[c->npage++] = x;
	}
	if (c->npage > 1)
		qsort(c->page, c->npage, sizeof(uint64), lion_code_cmp);
#ifdef USE_ASSERT_CHECKING
	for (int i = 1; i < c->npage; i++)
		Assert(c->page[i - 1] < c->page[i]);
#endif

	c->ipage = 1;
	c->code = c->page[0];
	c->hascode = true;
}

/* Move to the next record, or to the end of the column. */
static void
lion_cursor_next_record(LionRunCursor *c)
{
	LionRunHeader h;

	c->hascode = false;
	c->resort = false;
	c->havepeek = false;
	c->npage = c->ipage = 0;
	c->last = 0;

	if (c->tape == NULL)
	{
		LionAccEntry *e;

		if (c->nextitem >= c->nitems)
		{
			c->eos = true;
			return;
		}
		e = c->items[c->nextitem].entry;
		c->flags = c->itemflags ? c->itemflags[c->nextitem] : 0;
		c->nextitem++;

		c->eos = false;
		c->kind = e->kind;
		c->hash = e->hash;
		c->key = e->key;
		c->raw = lion_acc_raw(c->state, e);
		c->rawlen = e->keylen;
		c->lastchunk = true;
		if (e->buf == NULL)
		{
			c->p = c->single;
			c->end = c->single + lion_varbyte_put(c->single, e->mincode);
		}
		else
		{
			c->p = e->buf;
			c->end = e->buf + e->len;
		}
		if (e->unsorted)
		{
			c->resort = true;
			if (c->page == NULL)
				c->page = MemoryContextAlloc(c->cxt,
											 sizeof(uint64) * LION_PAGE_CODES);
		}
		lion_cursor_step(c);
		Assert(c->hascode && c->code == e->mincode);
		c->firstcode = c->code;
		return;
	}

	/* whatever the merge left of the record before */
	while (!c->lastchunk)
	{
		c->p = c->end;
		lion_cursor_fill(c);
	}

	lion_tape_read(c->tape, &h, sizeof(h));
	if (h.kind == LION_RUN_END)
	{
		c->eos = true;
		return;
	}
	if ((h.kind != LION_KIND_NULL && h.kind != LION_KIND_EMPTY &&
		 h.kind != LION_KIND_VALUE) ||
		(h.kind == LION_KIND_VALUE) != (h.keylen > 0) ||
		h.keylen > LION_MAX_KEY_SIZE)
		elog(ERROR, "lion index build: malformed record in a spilled run");

	c->eos = false;
	c->kind = h.kind;
	c->flags = h.flags;
	c->hash = h.hash;
	c->rawlen = h.keylen;
	if (h.keylen > 0)
	{
		if (c->keycap < h.keylen)
		{
			if (c->keybuf != NULL)
				pfree(c->keybuf);
			c->keycap = Max(MAXALIGN(h.keylen), 64);
			c->keybuf = MemoryContextAlloc(c->cxt, c->keycap);
		}
		lion_tape_read(c->tape, c->keybuf, h.keylen);
		c->raw = c->keybuf;
		c->key = lion_fetch_key(c->state, c->keybuf);
	}
	else
	{
		c->raw = NULL;
		c->key = (Datum) 0;
	}

	c->p = c->end = c->cbuf;
	c->lastchunk = false;
	lion_cursor_step(c);
	if (!c->hascode)
		elog(ERROR, "lion index build: a spilled key has no codes");
	c->firstcode = c->code;
}

/* ---------------------------------------------------------------------
 * Groups
 * --------------------------------------------------------------------- */

int
lion_spool_group_size(const LionSpoolGroup *group)
{
	return group->nentries;
}

const LionSpoolEntry *
lion_spool_group_entry(const LionSpoolGroup *group, int i)
{
	Assert(i >= 0 && i < group->nentries);
	return &group->entries[i];
}

/*
 * The group's next code, ascending across all its entries, and the entry it
 * belongs to.
 *
 * The codes of one entry come from several inputs when the key was spilled
 * more than once or when several participants of a parallel build saw it.
 * They are merged by taking the input with the smallest code and staying on
 * it while its codes stay below the next input's, which makes the runs of a
 * serial scan - whose codes follow one another - a concatenation, and a
 * parallel scan's participants - whose codes interleave one block chunk at a
 * time - one comparison per chunk rather than per code.
 */
bool
lion_spool_group_next(LionSpoolGroup *group, int *entry, uint64 *code)
{
	int			i;

	if (!group->materialised)
	{
		LionRunCursor *c;

		*entry = 0;
		if (group->ncurs == 1)
		{
			c = group->curs[0];
			if (!c->hascode)
				return false;
			*code = c->code;
			lion_cursor_step(c);
			return true;
		}

		c = (group->active >= 0) ? group->curs[group->active] : NULL;
		if (c == NULL || !c->hascode ||
			(group->havelimit && c->code > group->limit))
		{
			int			best = -1;
			uint64		second = 0;
			bool		havesecond = false;

			for (i = 0; i < group->ncurs; i++)
			{
				LionRunCursor *x = group->curs[i];

				if (!x->hascode)
					continue;
				if (best < 0 || x->code < group->curs[best]->code)
				{
					if (best >= 0)
					{
						second = group->curs[best]->code;
						havesecond = true;
					}
					best = i;
				}
				else if (!havesecond || x->code < second)
				{
					second = x->code;
					havesecond = true;
				}
			}
			if (best < 0)
				return false;
			Assert(!havesecond || second != group->curs[best]->code);
			group->active = best;
			group->limit = second;
			group->havelimit = havesecond;
			c = group->curs[best];
		}
		*code = c->code;
		lion_cursor_step(c);
		return true;
	}

	/* a materialised group: the same, over its arrays */
	{
		int			best = -1;

		if (group->active >= 0 &&
			group->pos[group->active] < group->ncodes[group->active] &&
			(!group->havelimit ||
			 group->codes[group->active][group->pos[group->active]] < group->limit))
			best = group->active;
		else
		{
			uint64		second = 0;
			bool		havesecond = false;

			for (i = 0; i < group->nentries; i++)
			{
				uint64		x;

				if (group->pos[i] >= group->ncodes[i])
					continue;
				x = group->codes[i][group->pos[i]];
				if (best < 0 || x < group->codes[best][group->pos[best]])
				{
					if (best >= 0)
					{
						second = group->codes[best][group->pos[best]];
						havesecond = true;
					}
					best = i;
				}
				else if (!havesecond || x < second)
				{
					second = x;
					havesecond = true;
				}
			}
			if (best < 0)
				return false;
			group->active = best;
			group->limit = second;
			group->havelimit = havesecond;
		}
		*entry = best;
		*code = group->codes[best][group->pos[best]++];
		return true;
	}
}

/* ---------------------------------------------------------------------
 * The merge
 * --------------------------------------------------------------------- */

static void
lion_merge_init(LionMerge *m, LionState *state, MemoryContext cxt)
{
	m->state = state;
	m->cxt = cxt;
	if (state->ordered)
	{
		memset(&m->ssup, 0, sizeof(m->ssup));
		m->ssup.ssup_cxt = CurrentMemoryContext;
		m->ssup.ssup_collation = state->collation;
		m->ssup.ssup_nulls_first = false;
		m->ssup.abbreviate = false;
		PrepareSortSupportFromOrderingOp(state->ltopr, &m->ssup);
	}
}

/*
 * The directory order's prefix (DESIGN.md §21): the kind, then an ordered
 * column's comparison or an unordered one's hash.  Two records that tie on it
 * may be one key, and only the opclass equality says whether they are.
 */
static inline int
lion_merge_prefix_cmp(LionMerge *m, const LionRunCursor *x,
					  const LionRunCursor *y)
{
	if (x->kind != y->kind)
		return x->kind < y->kind ? -1 : 1;
	if (x->kind != LION_KIND_VALUE)
		return 0;
	if (m->state->ordered)
		return ApplySortComparator(x->key, false, y->key, false, &m->ssup);
	return (x->hash > y->hash) - (x->hash < y->hash);
}

/* binaryheap keeps its LARGEST element first, so this is backwards. */
static int
lion_merge_heap_cmp(Datum a, Datum b, void *arg)
{
	LionMerge  *m = (LionMerge *) arg;
	const LionRunCursor *x = (const LionRunCursor *) DatumGetPointer(a);
	const LionRunCursor *y = (const LionRunCursor *) DatumGetPointer(b);
	int			c = lion_merge_prefix_cmp(m, x, y);

	if (c == 0)
		c = (x->index > y->index) - (x->index < y->index);
	return -c;
}

typedef struct LionMatRecord
{
	int			kind;
	uint32		hash;
	char	   *raw;
	Size		rawlen;
	Datum		key;
	uint64	   *codes;
	uint64		ncodes;
	int			distinct;		/* the entry it is part of */
} LionMatRecord;

typedef struct LionMatEntry
{
	LionMatRecord *rep;			/* the record with the smallest code */
	uint64		ncodes;
	uint64	   *codes;
} LionMatEntry;

typedef struct LionMatSortArg
{
	LionMerge  *m;
	LionMatEntry *ents;
} LionMatSortArg;

static int
lion_mat_entry_cmp(const void *a, const void *b, void *arg)
{
	LionMatSortArg *s = (LionMatSortArg *) arg;
	const LionMatRecord *x = s->ents[*(const int *) a].rep;
	const LionMatRecord *y = s->ents[*(const int *) b].rep;
	int			c;

	if (x->kind != y->kind)
		return x->kind < y->kind ? -1 : 1;
	if (x->kind != LION_KIND_VALUE)
		return 0;
	if (s->m->state->ordered)
	{
		c = ApplySortComparator(x->key, false, y->key, false, &s->m->ssup);
		if (c != 0)
			return c;
	}
	return lion_tail_cmp(x->hash, x->raw, x->rawlen, y->hash, y->raw, y->rawlen);
}

/*
 * The group's inputs hold more than one key between them: an unordered
 * opclass's hash collided, within one run or across several.  Read every
 * record of the group into memory, gather the records of each distinct key,
 * and order the keys by the directory's full order.  Collisions are rare and
 * this is the only place a key's codes are ever all in memory at once.
 */
static void
lion_merge_materialise(LionMerge *m, LionSpoolGroup *g, LionRunCursor **grp,
					   int ngrp)
{
	MemoryContext old = MemoryContextSwitchTo(m->cxt);
	LionMatRecord *recs;
	int			nrecs = 0;
	int			maxrecs = 8;
	LionMatEntry *ents;
	int			nents = 0;
	int		   *order;
	LionMatSortArg sarg;
	int			i;
	int			j;

	recs = palloc(sizeof(LionMatRecord) * maxrecs);
	for (i = 0; i < ngrp; i++)
	{
		LionRunCursor *c = grp[i];

		for (;;)
		{
			LionMatRecord *r;
			uint64		cap = 64;

			if (nrecs >= maxrecs)
			{
				maxrecs *= 2;
				recs = repalloc(recs, sizeof(LionMatRecord) * maxrecs);
			}
			r = &recs[nrecs++];
			r->kind = c->kind;
			r->hash = c->hash;
			r->rawlen = c->rawlen;
			r->raw = palloc(Max(MAXALIGN(c->rawlen), 8));
			if (c->rawlen > 0)
				memcpy(r->raw, c->raw, c->rawlen);
			r->key = (c->kind == LION_KIND_VALUE) ?
				lion_fetch_key(m->state, r->raw) : (Datum) 0;
			r->codes = MemoryContextAllocHuge(m->cxt, sizeof(uint64) * cap);
			r->ncodes = 0;
			while (c->hascode)
			{
				if (r->ncodes >= cap)
				{
					cap *= 2;
					r->codes = repalloc_huge(r->codes, sizeof(uint64) * cap);
				}
				r->codes[r->ncodes++] = c->code;
				lion_cursor_step(c);
			}

			if ((c->flags & LION_RUN_TIED) == 0)
				break;
			lion_cursor_next_record(c);
			if (c->eos || lion_merge_prefix_cmp(m, c, grp[0]) != 0)
				elog(ERROR, "lion index build: a spilled run is out of order");
		}
	}

	/* one entry per distinct key */
	ents = palloc(sizeof(LionMatEntry) * nrecs);
	for (i = 0; i < nrecs; i++)
	{
		LionMatRecord *r = &recs[i];

		for (j = 0; j < nents; j++)
		{
			LionMatRecord *d = ents[j].rep;

			if (d->kind == r->kind &&
				(r->kind != LION_KIND_VALUE ||
				 lion_keys_equal(m->state, d->key, r->key)))
				break;
		}
		if (j == nents)
		{
			ents[nents].rep = r;
			ents[nents].ncodes = 0;
			nents++;
		}
		else if (r->codes[0] < ents[j].rep->codes[0])
			ents[j].rep = r;
		r->distinct = j;
		ents[j].ncodes += r->ncodes;
	}
	for (j = 0; j < nents; j++)
	{
		uint64		n = 0;
		int			nparts = 0;

		ents[j].codes = MemoryContextAllocHuge(m->cxt,
											   sizeof(uint64) * ents[j].ncodes);
		for (i = 0; i < nrecs; i++)
		{
			if (recs[i].distinct != j)
				continue;
			memcpy(ents[j].codes + n, recs[i].codes,
				   sizeof(uint64) * recs[i].ncodes);
			n += recs[i].ncodes;
			nparts++;
		}
		if (nparts > 1)
			qsort(ents[j].codes, n, sizeof(uint64), lion_code_cmp);
		for (i = 1; i < (int64) n; i++)
		{
			if (ents[j].codes[i - 1] >= ents[j].codes[i])
				elog(ERROR, "lion index build: a code appears twice under one key");
		}
	}

	order = palloc(sizeof(int) * nents);
	for (j = 0; j < nents; j++)
		order[j] = j;
	sarg.m = m;
	sarg.ents = ents;
	qsort_arg(order, nents, sizeof(int), lion_mat_entry_cmp, &sarg);

	g->materialised = true;
	g->nentries = nents;
	g->entries = palloc(sizeof(LionSpoolEntry) * nents);
	g->codes = palloc(sizeof(uint64 *) * nents);
	g->ncodes = palloc(sizeof(uint64) * nents);
	g->pos = palloc0(sizeof(uint64) * nents);
	g->active = -1;
	g->havelimit = false;
	for (j = 0; j < nents; j++)
	{
		LionMatEntry *e = &ents[order[j]];

		g->entries[j].kind = e->rep->kind;
		g->entries[j].hash = e->rep->hash;
		g->entries[j].key = e->rep->key;
		g->entries[j].raw = e->rep->raw;
		g->entries[j].rawlen = e->rep->rawlen;
		g->codes[j] = e->codes;
		g->ncodes[j] = e->ncodes;
	}

	MemoryContextSwitchTo(old);
}

/*
 * Merge one column's inputs, each already on its first record, and hand
 * every entry to emit in directory order.
 */
static void
lion_merge_column(LionMerge *m, LionRunCursor **in, int nin,
				  LionSpoolEmit emit, void *arg)
{
	binaryheap *heap;
	LionRunCursor **grp;
	LionSpoolGroup group;
	int			i;

	if (nin == 0)
		return;

	heap = binaryheap_allocate(nin, lion_merge_heap_cmp, m);
	grp = palloc(sizeof(LionRunCursor *) * nin);
	for (i = 0; i < nin; i++)
	{
		if (!in[i]->eos)
			binaryheap_add_unordered(heap, PointerGetDatum(in[i]));
	}
	binaryheap_build(heap);

	while (!binaryheap_empty(heap))
	{
		int			ngrp = 0;
		bool		streaming = true;
		int			which;
		uint64		code;

		grp[ngrp++] = (LionRunCursor *) DatumGetPointer(binaryheap_remove_first(heap));
		while (!binaryheap_empty(heap) &&
			   lion_merge_prefix_cmp(m, (LionRunCursor *) DatumGetPointer(binaryheap_first(heap)),
									 grp[0]) == 0)
			grp[ngrp++] = (LionRunCursor *) DatumGetPointer(binaryheap_remove_first(heap));

		/*
		 * The common case: one key, which each input in the group holds in
		 * one record.  Its codes are streamed straight from the inputs.
		 */
		for (i = 0; i < ngrp && streaming; i++)
		{
			if (grp[i]->flags & LION_RUN_TIED)
				streaming = false;
			else if (i > 0 && grp[0]->kind == LION_KIND_VALUE &&
					 !lion_keys_equal(m->state, grp[0]->key, grp[i]->key))
				streaming = false;
		}

		memset(&group, 0, sizeof(group));
		if (streaming)
		{
			LionRunCursor *rep = grp[0];

			for (i = 1; i < ngrp; i++)
			{
				if (grp[i]->firstcode < rep->firstcode)
					rep = grp[i];
			}
			group.nentries = 1;
			group.entries = &group.one;
			group.one.kind = rep->kind;
			group.one.hash = rep->hash;
			group.one.key = rep->key;
			group.one.raw = rep->raw;
			group.one.rawlen = rep->rawlen;
			group.curs = grp;
			group.ncurs = ngrp;
			group.active = -1;
		}
		else
			lion_merge_materialise(m, &group, grp, ngrp);

		emit(arg, &group);

		/* whatever emit did not take is not wanted */
		while (lion_spool_group_next(&group, &which, &code))
			;
		if (group.materialised)
			MemoryContextReset(m->cxt);

		for (i = 0; i < ngrp; i++)
		{
			lion_cursor_next_record(grp[i]);
			if (!grp[i]->eos)
				binaryheap_add(heap, PointerGetDatum(grp[i]));
		}

		CHECK_FOR_INTERRUPTS();
	}

	binaryheap_free(heap);
	pfree(grp);
}

/* An emit that writes the entries to a run instead. */
static void
lion_run_emit(void *arg, LionSpoolGroup *group)
{
	LionRunWriter *w = (LionRunWriter *) arg;
	int			which;
	uint64		code;
	int			i;

	if (!group->materialised)
	{
		const LionSpoolEntry *e = &group->entries[0];

		lion_run_begin_record(w, (uint8) e->kind, 0, e->hash, e->raw,
							  e->rawlen);
		while (lion_spool_group_next(group, &which, &code))
			lion_run_put_code(w, code);
		lion_run_end_record(w);
		return;
	}

	for (i = 0; i < group->nentries; i++)
	{
		const LionSpoolEntry *e = &group->entries[i];
		uint64		j;

		lion_run_begin_record(w, (uint8) e->kind,
							  (i + 1 < group->nentries) ? LION_RUN_TIED : 0,
							  e->hash, e->raw, e->rawlen);
		for (j = 0; j < group->ncodes[i]; j++)
			lion_run_put_code(w, group->codes[i][j]);
		group->pos[i] = group->ncodes[i];
		lion_run_end_record(w);
	}
}

/* ---------------------------------------------------------------------
 * The spool
 * --------------------------------------------------------------------- */

LionSpool *
lion_spool_begin(LionIndexState *ix, Size membytes, SharedFileSet *fileset,
				 int filenum)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "lion build spool",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	LionSpool  *sp = palloc0(sizeof(LionSpool));
	int			c;

	sp->ix = ix;
	sp->ncols = ix->ncolumns;
	sp->cxt = cxt;
	sp->rowcxt = AllocSetContextCreate(cxt, "lion build spool row",
									   ALLOCSET_DEFAULT_SIZES);
	sp->mergecxt = AllocSetContextCreate(cxt, "lion build spool merge",
										 ALLOCSET_DEFAULT_SIZES);
	sp->membytes = Max(membytes, (Size) 64 * 1024);

	/*
	 * A merge reads its runs through buffers of readbuf bytes, and reads at
	 * most maxorder of them at once, so that the buffers of one merge pass
	 * take no more than half the budget; more runs than that are merged in
	 * more than one pass (lion_spool_reduce_runs()).
	 */
	sp->readbuf = Max((Size) BLCKSZ, Min((Size) BLCKSZ * 8, sp->membytes / 16));
	sp->maxorder = (int) Max(6, Min(512, sp->membytes / 2 / sp->readbuf));

	sp->fileset = fileset;
	sp->filenum = filenum;
	sp->cols = palloc0(sizeof(LionAccCol) * sp->ncols);
	for (c = 0; c < sp->ncols; c++)
	{
		LionAccCol *col = &sp->cols[c];

		col->state = &ix->cols[c];

		/*
		 * Blocks sized to the budget: a context's blocks double up to its
		 * largest size, and a small budget would otherwise be gone in one
		 * block.
		 */
		col->cxt = AllocSetContextCreate(cxt, "lion build spool column",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 Max(ALLOCSET_DEFAULT_INITSIZE,
											 Min(ALLOCSET_DEFAULT_MAXSIZE,
												 sp->membytes / 16)));
		col->arenablock = Max((Size) 1024,
							  Min(LION_ARENA_BLOCK, sp->membytes / 32));
		col->maxruns = 8;
		col->runs = palloc(sizeof(LogicalTape *) * col->maxruns);
		lion_acc_reset(col);
	}

	/* a participant's file has to exist for the leader to import it */
	if (fileset != NULL)
		sp->tapes = LogicalTapeSetCreate(false, fileset, filenum);

	MemoryContextSwitchTo(old);
	return sp;
}

static void
lion_spool_tapes(LionSpool *sp)
{
	if (sp->tapes == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(sp->cxt);

		PrepareTempTablespaces();
		sp->tapes = LogicalTapeSetCreate(false, NULL, -1);
		MemoryContextSwitchTo(old);
	}
}

static Size
lion_spool_memory(LionSpool *sp)
{
	Size		total = 0;
	int			c;

	for (c = 0; c < sp->ncols; c++)
		total += lion_acc_used(&sp->cols[c]);
	return total;
}

/*
 * A new tape in the spool's set.  Its struct, and the write buffer the first
 * write allocates, belong to the spool's own context: a tape outlives the
 * merge that writes it, whose context is reset after each one.  (The read
 * buffer a tape allocates at its first read is freed by LogicalTapeClose(),
 * which every merge does before its context goes.)
 */
static LogicalTape *
lion_spool_new_tape(LionSpool *sp)
{
	MemoryContext old = MemoryContextSwitchTo(sp->cxt);
	LogicalTape *tape = LogicalTapeCreate(sp->tapes);

	LogicalTapeWrite(tape, NULL, 0);
	MemoryContextSwitchTo(old);
	return tape;
}

static void
lion_spool_add_run(LionSpool *sp, LionAccCol *col, LogicalTape *tape)
{
	if (col->nruns >= col->maxruns)
	{
		MemoryContext old = MemoryContextSwitchTo(sp->cxt);

		col->maxruns *= 2;
		col->runs = repalloc(col->runs, sizeof(LogicalTape *) * col->maxruns);
		MemoryContextSwitchTo(old);
	}
	col->runs[col->nruns++] = tape;
}

/* Write a column's entries to a new run, and forget them. */
static void
lion_spool_spill_column(LionSpool *sp, LionAccCol *col)
{
	MemoryContext old;
	LionSortItem *items;
	uint8	   *flags;
	int			n;
	LogicalTape *tape;
	LionRunWriter *w;
	int			i;

	if (col->nentries == 0)
		return;

	lion_spool_tapes(sp);
	old = MemoryContextSwitchTo(sp->mergecxt);
	items = lion_acc_sorted(col, &flags, &n);
	tape = lion_spool_new_tape(sp);
	w = palloc(sizeof(LionRunWriter));
	w->tape = tape;
	for (i = 0; i < n; i++)
	{
		LionAccEntry *e = items[i].entry;

		lion_run_begin_record(w, e->kind, flags ? flags[i] : 0, e->hash,
							  lion_acc_raw(col->state, e), e->keylen);
		if (e->buf == NULL)
			lion_run_put_code(w, e->mincode);
		else if (!e->unsorted)
			lion_run_put_encoded(w, e->buf, e->len);
		else
		{
			LionRunCursor c;

			lion_cursor_init_memory(&c, col->state, 0, &items[i], NULL, 1);
			lion_cursor_next_record(&c);
			while (c.hascode)
			{
				lion_run_put_code(w, c.code);
				lion_cursor_step(&c);
			}
		}
		lion_run_end_record(w);
	}
	lion_run_end_column(tape);

	/* the write buffer goes now, the read buffer comes with the first read */
	LogicalTapeRewindForRead(tape, sp->readbuf);
	MemoryContextSwitchTo(old);
	MemoryContextReset(sp->mergecxt);

	lion_spool_add_run(sp, col, tape);
	lion_acc_reset(col);
	sp->nspills++;
}

/*
 * Over the budget: spill the largest columns until half of it is free.  A
 * column of few keys takes little and is left alone, so it ends up with few
 * runs or none.
 */
static void
lion_spool_spill_largest(LionSpool *sp)
{
	for (;;)
	{
		Size		total = 0;
		Size		largest = 0;
		LionAccCol *victim = NULL;
		int			c;

		for (c = 0; c < sp->ncols; c++)
		{
			LionAccCol *col = &sp->cols[c];
			Size		size = lion_acc_used(col);

			total += size;
			if (col->nentries > 0 && size > largest)
			{
				largest = size;
				victim = col;
			}
		}
		if (victim == NULL || total <= sp->membytes / 2)
			break;
		lion_spool_spill_column(sp, victim);
	}
}

void
lion_spool_add(LionSpool *sp, ItemPointer tid, Datum *values, bool *isnull)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	MemoryContext old;
	uint64		code;
	int			c;

	lion_check_key_offset(tid);

	/* only ever between two heap pages (see the file header) */
	if (!sp->haveblk || blk != sp->curblk)
	{
		if (sp->haveblk && blk < sp->curblk)
		{
			for (c = 0; c < sp->ncols; c++)
				lion_spool_spill_column(sp, &sp->cols[c]);
		}
		else if (lion_spool_memory(sp) > sp->membytes)
			lion_spool_spill_largest(sp);
		sp->curblk = blk;
		sp->haveblk = true;
	}

	code = lion_tid_to_code(tid);
	old = MemoryContextSwitchTo(sp->rowcxt);

	/* One row contributes to every key column (DESIGN.md §24). */
	for (c = 0; c < sp->ncols; c++)
	{
		LionAccCol *col = &sp->cols[c];
		LionState  *cs = col->state;

		/* A NULL goes to the column's reserved NULL entry (DESIGN.md §14). */
		if (isnull[c])
			lion_acc_add_reserved(sp, col, LION_KIND_NULL, code);
		else if (cs->multikey)
		{
			/*
			 * DESIGN.md §17: one row, many keys.  A row the opclass extracts
			 * nothing from - an empty array, a tsvector with no lexemes -
			 * goes into the reserved EMPTY entry, so that a scan that has to
			 * look at every indexed row (`tags @> '{}'`) can still find it.
			 */
			Datum	   *keys;
			int			nkeys = lion_extract_value(cs, values[c], &keys);
			int			i;

			if (nkeys == 0)
				lion_acc_add_reserved(sp, col, LION_KIND_EMPTY, code);
			for (i = 0; i < nkeys; i++)
				lion_acc_add(sp, col, keys[i], code);
		}
		else
		{
			Datum		key = values[c];

			/*
			 * An out-of-line or compressed value is expanded once here rather
			 * than by every function it meets.  A short header is left as it
			 * is: the hash and equality functions take it as it comes, and
			 * lion_store_key() gives a new key its full header.
			 */
			if (cs->typlen == -1 &&
				(VARATT_IS_EXTERNAL(DatumGetPointer(key)) ||
				 VARATT_IS_COMPRESSED(DatumGetPointer(key))))
				key = PointerGetDatum(PG_DETOAST_DATUM(key));

			lion_acc_add(sp, col, key, code);
		}
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(sp->rowcxt);
}

double
lion_spool_ntids(const LionSpool *sp)
{
	return sp->ntids;
}

int
lion_spool_nruns(const LionSpool *sp)
{
	return sp->nspills;
}

/*
 * Leave at most maxorder inputs for the column's last merge, merging its
 * oldest runs into one as often as it takes.  Which runs are merged does not
 * matter: a merge orders codes, not runs.
 */
static void
lion_spool_reduce_runs(LionSpool *sp, LionAccCol *col, bool inmemory)
{
	while (col->nruns + (inmemory ? 1 : 0) > sp->maxorder)
	{
		int			k = Min(sp->maxorder, col->nruns);
		MemoryContext old = MemoryContextSwitchTo(sp->mergecxt);
		LionRunCursor **in = palloc(sizeof(LionRunCursor *) * k);
		LionRunWriter *w = palloc(sizeof(LionRunWriter));
		LogicalTape *tape = lion_spool_new_tape(sp);
		LionMerge	m;
		int			i;

		for (i = 0; i < k; i++)
		{
			in[i] = palloc(sizeof(LionRunCursor));
			lion_cursor_init_tape(in[i], col->state, i, col->runs[i]);
			lion_cursor_next_record(in[i]);
		}
		w->tape = tape;
		lion_merge_init(&m, col->state,
						AllocSetContextCreate(sp->mergecxt,
											  "lion build spool group",
											  ALLOCSET_DEFAULT_SIZES));
		lion_merge_column(&m, in, k, lion_run_emit, w);
		lion_run_end_column(tape);
		LogicalTapeRewindForRead(tape, sp->readbuf);

		for (i = 0; i < k; i++)
			LogicalTapeClose(col->runs[i]);
		memmove(col->runs, col->runs + k,
				sizeof(LogicalTape *) * (col->nruns - k));
		col->nruns -= k;
		MemoryContextSwitchTo(old);
		MemoryContextReset(sp->mergecxt);
		lion_spool_add_run(sp, col, tape);
	}
}

/* Merge a column's runs and what it has in memory into emit, and forget it. */
static void
lion_spool_merge_column(LionSpool *sp, int colno, LionSpoolEmit emit,
						void *arg)
{
	LionAccCol *col = &sp->cols[colno];
	bool		inmemory = (col->nentries > 0);
	MemoryContext old;
	LionRunCursor **in;
	int			nin = 0;
	LionMerge	m;
	int			i;

	if (col->nruns > 0)
		lion_spool_reduce_runs(sp, col, inmemory);

	old = MemoryContextSwitchTo(sp->mergecxt);
	in = palloc(sizeof(LionRunCursor *) * (col->nruns + 1));
	for (i = 0; i < col->nruns; i++)
	{
		in[nin] = palloc(sizeof(LionRunCursor));
		lion_cursor_init_tape(in[nin], col->state, nin, col->runs[i]);
		lion_cursor_next_record(in[nin]);
		nin++;
	}
	if (inmemory)
	{
		uint8	   *flags;
		int			n;
		LionSortItem *items = lion_acc_sorted(col, &flags, &n);

		in[nin] = palloc(sizeof(LionRunCursor));
		lion_cursor_init_memory(in[nin], col->state, nin, items, flags, n);
		lion_cursor_next_record(in[nin]);
		nin++;
	}

	lion_merge_init(&m, col->state,
					AllocSetContextCreate(sp->mergecxt, "lion build spool group",
										  ALLOCSET_DEFAULT_SIZES));
	lion_merge_column(&m, in, nin, emit, arg);

	for (i = 0; i < col->nruns; i++)
		LogicalTapeClose(col->runs[i]);
	col->nruns = 0;
	MemoryContextSwitchTo(old);
	MemoryContextReset(sp->mergecxt);
	lion_acc_reset(col);
}

void
lion_spool_emit_column(LionSpool *sp, int col, LionSpoolEmit emit, void *arg)
{
	Assert(col >= 0 && col < sp->ncols);
	lion_spool_merge_column(sp, col, emit, arg);
}

/*
 * A parallel participant's output: every column's entries, in directory
 * order, one section per column in column order, on one tape frozen for the
 * leader to import.
 */
void
lion_spool_export(LionSpool *sp, TapeShare *share)
{
	LogicalTape *out;
	LionRunWriter *w;
	int			c;

	MemoryContext old;

	Assert(sp->fileset != NULL && sp->tapes != NULL);
	out = lion_spool_new_tape(sp);
	w = MemoryContextAlloc(sp->cxt, sizeof(LionRunWriter));
	w->tape = out;
	for (c = 0; c < sp->ncols; c++)
	{
		lion_spool_merge_column(sp, c, lion_run_emit, w);
		lion_run_end_column(out);
	}
	old = MemoryContextSwitchTo(sp->cxt);
	LogicalTapeFreeze(out, share);
	MemoryContextSwitchTo(old);
}

void
lion_spool_end(LionSpool *sp)
{
	if (sp->tapes != NULL)
		LogicalTapeSetClose(sp->tapes);
	MemoryContextDelete(sp->cxt);
}

/* ---------------------------------------------------------------------
 * The leader of a parallel build
 * --------------------------------------------------------------------- */

LionSpoolReader *
lion_spool_reader_begin(LionIndexState *ix, SharedFileSet *fileset,
						int nparticipants, const int *filenums,
						TapeShare *shares, Size membytes)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "lion build spool reader",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	LionSpoolReader *rd = palloc0(sizeof(LionSpoolReader));
	Size		readbuf;
	int			i;

	rd->ix = ix;
	rd->cxt = cxt;
	rd->mergecxt = AllocSetContextCreate(cxt, "lion build spool merge",
										 ALLOCSET_DEFAULT_SIZES);
	rd->nparts = nparticipants;
	rd->tapes = LogicalTapeSetCreate(false, fileset, -1);
	readbuf = Max((Size) BLCKSZ,
				  Min((Size) BLCKSZ * 32, membytes / 2 / Max(nparticipants, 1)));
	rd->curs = palloc(sizeof(LionRunCursor *) * nparticipants);
	for (i = 0; i < nparticipants; i++)
	{
		LogicalTape *tape = LogicalTapeImport(rd->tapes, filenums[i],
											  &shares[i]);

		LogicalTapeRewindForRead(tape, readbuf);
		/* the read buffer, now, in the reader's context (see above) */
		LogicalTapeRead(tape, NULL, 0);
		rd->curs[i] = palloc(sizeof(LionRunCursor));
		lion_cursor_init_tape(rd->curs[i], &ix->cols[0], i, tape);
	}
	LogicalTapeSetForgetFreeSpace(rd->tapes);

	MemoryContextSwitchTo(old);
	return rd;
}

/*
 * One column's entries from every participant.  Each participant's tape holds
 * the columns in order, so the cursors carry on from where the column before
 * left them.
 */
void
lion_spool_reader_emit_column(LionSpoolReader *rd, int col,
							  LionSpoolEmit emit, void *arg)
{
	MemoryContext old = MemoryContextSwitchTo(rd->mergecxt);
	LionMerge	m;
	int			i;

	for (i = 0; i < rd->nparts; i++)
	{
		rd->curs[i]->state = &rd->ix->cols[col];
		lion_cursor_next_record(rd->curs[i]);
	}
	lion_merge_init(&m, &rd->ix->cols[col],
					AllocSetContextCreate(rd->mergecxt, "lion build spool group",
										  ALLOCSET_DEFAULT_SIZES));
	lion_merge_column(&m, rd->curs, rd->nparts, emit, arg);

	MemoryContextSwitchTo(old);
	MemoryContextReset(rd->mergecxt);
}

void
lion_spool_reader_end(LionSpoolReader *rd)
{
	LogicalTapeSetClose(rd->tapes);
	MemoryContextDelete(rd->cxt);
}
