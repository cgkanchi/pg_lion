# pg_lion: a roaring-bitmap inverted index AM for PostgreSQL — design v0

Status: prototype targeting PostgreSQL master (20devel). Built and tested against the assert-enabled
install in `.local/pg` (`.local/pg/bin/pg_config`). This document is the single source of truth for
on-disk format, locking protocol, and module boundaries. Change it before changing the code.

Extension name: `pg_lion`. Access method name: `roaring`. C symbol prefix: `lion_` / `RBI`.

    CREATE EXTENSION pg_lion;
    CREATE INDEX ON fact USING lion (country);

## 1. Goals and non-goals for v0

Goals
- Single-column equality index whose posting lists are roaring-style containers (array / bitset / run).
- Bitmap scans (`amgetbitmap`) so the planner can use it in Bitmap Index Scan / BitmapAnd / BitmapOr.
- Correct under concurrent INSERT and VACUUM. Crash-safe via generic WAL.
- Bulk build via tuplesort. Inserts, VACUUM (ambulkdelete) that removes TIDs and shrinks containers.
- SQL-callable `lion_index_stats()` and `lion_index_verify()` for tests and debugging.
- Layered so that phase 2 can add: a visibility-map-interlocked `lion_index_count()` and a
  CustomScan that answers `count(*) ... GROUP BY key` from containers without visiting the heap.

Non-goals for v0 (documented limitations; NULL keys and IN lists arrived in v1, §14 and §15;
entry deletion and page recycling in §18)
- Multi-column indexes, INCLUDE columns, ordered scans, `amgettuple` (arrived in §29), parallel build/scan,
  fine-grained write concurrency (inserts serialize on the directory leaf that holds the key, §21),
  key sizes above 2000 bytes.

## 2. TID encoding

Every heap TID is mapped to a 64-bit code and split into a container key and a 15-bit low part.

    LION_OFFSET_BITS      = 9 at BLCKSZ 8192 (MaxHeapTuplesPerPage = 291 < 512)
                           10 at 16K (585), 11 at 32K (1169)   -- computed at compile time, see lion_tid.h
    code(tid)            = ((uint64) block << LION_OFFSET_BITS) | offset        -- 41 bits at 8K
    LION_CONTAINER_BITS   = 15
    ckey(code)           = code >> 15                                          -- uint32, block >> 6 at 8K
    lo(code)             = code & 0x7FFF                                       -- 0 .. 32767
    blocks per container = 1 << (15 - LION_OFFSET_BITS) = 64 at 8K

Offsets ≥ (1 << LION_OFFSET_BITS) are an ERROR at insert/build time ("table AM not supported").
The meta page records offset_bits and container_bits; opening an index with a mismatch is an ERROR.

**Table access methods: the heap only (implemented 2026-09-23).** The encoding has room for the
heap's line pointers and nothing else, and the count of §9 reads the heap's visibility map under
an interlock that is a statement about heap VACUUM. Another table AM may hand out TIDs of any
shape (citus_columnar's are stripe row numbers, with offsets far past 511) and keeps no visibility
map, or one that means something else. So a lion index may only sit on a table whose table AM
ROUTINE is the heap's: `lion_table_am_supported()` (lion_am.c) tests
`rel->rd_tableam == GetHeapamTableAmRoutine()`, the test the count's heap recheck already made to
choose its per-block path. The routine and not the AM's name or Oid, because
`CREATE ACCESS METHOD heap2 TYPE TABLE HANDLER heap_tableam_handler` is the heap under another
name and works unchanged, while a handler that returns anything else - even a copy of the heap's
callbacks - may have changed anything.

- **ambuild is the gate**: `lionbuild()` refuses first thing, rows or no rows, with
  FEATURE_NOT_SUPPORTED, `access method "lion" does not support table access method "<am>"`, a
  detail naming the table and a hint to use the heap. Every way an index comes to sit on a table
  builds it: CREATE INDEX, REINDEX, ALTER TABLE ... SET ACCESS METHOD (a rewrite that rebuilds the
  indexes, so a heap table with a lion index cannot move to another AM and the ALTER rolls back),
  a partition created, attached or indexed under a partitioned lion index (each leaf is built on
  its own, so one leaf of another AM fails the CREATE INDEX on the parent, the CREATE TABLE ...
  PARTITION OF ... USING, and the ATTACH), and on 17+ a partitioned table's SET ACCESS METHOD,
  which only sets the default for partitions created later. aminsert needs no check: a table's AM
  cannot change without that rewrite. Before this check an index build on a non-heap table failed
  somewhere inside: at lion_check_key_offset() for columnar's TIDs, in heap_getnext() ("only heap
  AM is supported") for a copied heap routine, or not at all for an AM with small offsets.
- **The count paths refuse as well**, for an index that got past the gate (none can today; the
  catalogs can be edited, and a later release may loosen the build rule): the planner declines a
  leaf of another AM in `lion_collect_targets()`, so one such partition declines the whole
  partitioned query (§10, §16); the executor's `lion_open_relation()` and the SQL counts'
  `lion_count_open_indexes()` raise the ambuild error.
- The other option the §23 citus_columnar item named, a per-AM TID encoding with the bitmap path
  only, was not taken: no table AM other than the heap is known to hand out heap-shaped TIDs, and
  a wider offset field is a format change that belongs with level (2) of that item, which needs
  its own visibility source anyway.
- Tests: `test/sql/tableam.sql` (the heap under another name: builds, pushdown and SQL counts on
  clean and dirty pages, SET ACCESS METHOD in both directions with the index rebuilt and verified,
  partitions of both AMs under one index) and `test/modules/lion_hooktest`'s `tableam` test under
  `make hookcheck`, whose `lion_heapcopy` AM returns a copy of the heap routine: every refusal
  above, and the three count paths declining or refusing after a table with lion indexes is moved
  to it by editing `pg_class.relam`. Not tested: a real non-heap AM (citus_columnar and
  TimescaleDB's compressed chunks are not in the test installs; §23).

Why not GIN's 11 offset bits: bitset containers would be 86% empty. Why 15 container bits and not 16:
a 16-bit bitset is 8192 bytes and cannot be a page item; 15 bits gives a 4096-byte bitset, so every
container fits on any page, and VACUUM can always fall back to a bitset when a run split would
otherwise grow a container past its slot.

## 3. Containers (module `lion_container.[ch]`, no backend dependencies beyond `c.h` + pg_bitutils)

    typedef struct LionContainer
    {
        uint32  ckey;          /* container key (code >> 15) */
        uint16  cardinality;   /* number of members, 0..32768 */
        uint8   type;          /* LION_CT_ARRAY=1, LION_CT_BITSET=2, LION_CT_RUN=3 */
        uint8   flags;         /* reserved, 0 */
        /* payload follows immediately (no padding; header is 8 bytes) */
    } LionContainer;

Payloads (all little values are host-endian uint16/uint64, like every other PG on-disk structure):
- ARRAY:  `uint16 lo[cardinality]`, strictly ascending. Max cardinality LION_ARRAY_MAX_CARD = 2048.
- BITSET: `uint64 words[512]` (4096 bytes, fixed), bit i set ⇔ lo i is a member.
- RUN:    `uint16 nruns; struct { uint16 start; uint16 len_minus_1; } runs[nruns]`, runs ascending and
          non-adjacent (merged). Max LION_RUN_MAX_NRUNS = 1023 (payload ≤ 4094 bytes).
- LION_CONTAINER_MAX_SIZE = 8 + 4096 = 4104 bytes. Invariant: every container ≤ this size.

Representation policy
- Insert into ARRAY beyond 2048 members ⇒ convert to BITSET. Insert into RUN that would exceed
  1023 runs ⇒ convert to BITSET.
- Remove from BITSET leaving ≤ 2048 members ⇒ convert to ARRAY. Remove from RUN that would exceed
  1023 runs (split) ⇒ convert to BITSET. Cardinality may reach 0; the caller deletes empty containers.
- `lion_container_optimize()` picks the smallest of the three representations. It is called at bulk
  build time for every container and by VACUUM after modifying a container. It is *not* called on
  every insert (inserts only enforce the size invariant), matching CRoaring's runOptimize semantics.
- Mutators operate on a caller-supplied buffer of LION_CONTAINER_MAX_SIZE bytes. Page code copies a
  container out of the page into such a buffer, mutates, and writes it back (in place when it fits).

Full API: `src/lion_container.h`. Unit tests: `test/unit/container_test.c` (`make unit`), which must
cover every type transition, boundary cardinalities (0, 1, 2047, 2048, 2049, 32767, 32768 members),
run merging/splitting, and set algebra against a brute-force 32768-bit reference.

## 4. Page layout (module `lion.h`, implemented in `lion_pages.c`)

All pages are standard PG pages (PageInit) with a special area:

    typedef struct LionPageOpaqueData
    {
        BlockNumber rightlink;     /* next page in this chain, or InvalidBlockNumber */
        uint32      minckey;       /* container pages: smallest ckey on page (0 if empty) */
        uint32      maxckey;       /* container pages: largest ckey on page (0 if empty) */
        uint32      owner_hash;    /* container pages: hash of the entry that owns the chain (§18) */
        BlockNumber owner_head;    /* container pages: head block of that chain (§18) */
        uint16      flags;         /* LION_PAGE_META | LION_PAGE_BUCKET | LION_PAGE_CONTAINER,
                                    * plus LION_PAGE_DELETED for a freed page (§18) */
        uint16      page_id;       /* LION_PAGE_ID = 0xFF87, for identification by inspection tools */
    } LionPageOpaqueData;          /* 24 bytes */

**Superseded in part by §24 (format version 6).** An entry tuple carries the KEY COLUMN it belongs
to in a `uint16 attno` at offset 20, where the 32-byte header had four bytes of alignment padding, so
the header and every item on every page stay exactly where they were. A `attno` of 0 belongs to the
minus-infinity downlink alone.

Block 0: meta page. Payload struct `LionMetaPageData` { magic 0x52424931, version 6, offset_bits,
container_bits, nbuckets, inline_limit, unused padding to 64 bytes }. Version 2 was the first that
indexes NULL keys (§14); version 3 (§18) added owner_hash/owner_head, which grows the special area
from 16 to 24 bytes and therefore moves every item on every page. An index of an older version is
structurally readable by nothing in this code, so opening one is an ERROR that asks for a REINDEX
rather than a wrong answer.

**Superseded by §21 (format version 4).** Everything in the next two paragraphs - the bucket
directory, `nbuckets`, `lion_bucket_of()` - is gone: the entry directory is a B-tree keyed by the
index key, entry tuples live on its LEAVES (which keep the flag name LION_PAGE_BUCKET), and the meta
page holds `root`/`height`/`dirpages` where it held `nbuckets`. What is unchanged is the entry tuple
itself and everything below it, so the paragraphs are kept for the entry layout they describe.

Blocks 1 .. nbuckets: bucket head pages. Bucket b for a key with 32-bit hash h is `h % nbuckets`
(`lion_bucket_of()` in lion.h, the single place that mapping lives); its head page is block `1 + b`.
A bucket is a rightlink chain of bucket pages holding entry tuples. nbuckets is NOT a power of two:
ambuild sizes it from the bytes the entries need (§5), and the `buckets` reloption means an exact
count.

Entry tuple (an item on a bucket page):

    typedef struct LionEntryTuple
    {
        uint32      hash;
        uint16      flags;          /* LION_ENTRY_INLINE or LION_ENTRY_CHAIN, plus
                                     * LION_ENTRY_NULLKEY for the NULL entry (§14) */
        uint16      keylen;         /* bytes of key data stored (0 for the NULL entry) */
        BlockNumber head;           /* CHAIN: first container page; INLINE: InvalidBlockNumber */
        BlockNumber tail;           /* CHAIN: last container page (append hint) */
        uint32      ncontainers;    /* containers in this key's posting set */
        uint64      ntids;          /* members in this key's posting set */
        /* key data: keylen bytes, then MAXALIGN padding */
        /* INLINE only: containers back to back, ascending ckey, total bytes = item size - offset */
    } LionEntryTuple;                /* header is 32 bytes (LION_ENTRY_HDRSZ; ntids forces padding) */

Key data is stored with datumCopy semantics: by-value types as a full `Datum` (8 bytes);
fixed-length by-reference types as typlen bytes; varlena as a detoasted, 4-byte-header varlena;
cstring as strlen+1 bytes. Keys larger than LION_MAX_KEY_SIZE = 2000 bytes are an ERROR.
Entry lookup compares `hash`, then calls the opclass equality operator's function (strategy 1 of the
opfamily, looked up once per relation and cached in `rd_amcache`) with the index collation.

INLINE entries hold their containers in the entry tuple while the payload is ≤ `inline_limit`
bytes (reloption, default 4096 = LION_MAX_INLINE_LIMIT). When an insert would exceed that, the entry
*spills*: allocate a container page, move the containers there, set head = tail = that page, flags =
CHAIN (keeping every LION_ENTRY_RESERVED bit: a key-less entry that loses its flag becomes an
ordinary entry with a zero-length key that no reader looks for, so the next row of its kind starts a
second one - the 2026-09-20 review reproduced exactly that), and shrink the entry tuple. Entries
never convert back from CHAIN to INLINE. The default is the maximum on purpose: a CHAIN posting set
owns whole container pages, so a key whose set is a few hundred bytes costs a whole page once it
spills (§13 measured a 20000-key index at 164 MB with a 1024-byte limit against 35 MB with 4096).

**Superseded in part by §22 (format version 5).** A CHAIN entry's container pages are the LEAVES of
a B-tree over container keys now: the entry's `head` is that tree's ROOT, and internal posting pages
(the same page kind, told apart by `level` > 0) hold downlinks above them. Everything the next three
paragraphs say about a leaf - the items, the ordering, min/max, splits, free space, growth slack -
is unchanged, and so is the right-link list they form; what changes is only how a container key is
turned into a block, and §22 says so where it does.

Container pages (per key, CHAIN entries): items are LionContainer structs, ascending ckey within a
page; all ckeys on page P are smaller than all ckeys on P.rightlink. `minckey`/`maxckey` in the
special area are maintained on every change. Page P owns free space like any heap/index page; use
PageAddItemExtended (with explicit offset to keep order), PageIndexTupleOverwrite (handles size
change), PageIndexTupleDeleteNoCompact/PageIndexMultiDelete + PageRepairFragmentation as needed.

Locating the page for a ckey (`lion_chain_find_page`): a DESCENT of the posting tree since §22. The
insert path still tries the append case first - the set's last leaf, taken with the exclusive lock
the insert needs anyway - and falls back to the descent for anything else; an empty tail has minckey
0 and must not be trusted, so it falls through too.

Growth (`lion_chain_put_container`): overwrite in place if the page has room; otherwise *split* page P:
move the items at/after the insert position to a freshly allocated page N linked after P; if the
container still does not fit on P, place it alone on a second new page M linked between P and N
(P → M → N → old right). This guarantees progress in one WAL record (P, N, M, entry page = 4 buffers,
the GenericXLog maximum). Items only ever move right and only to pages that are linked immediately
right of the page they came from - which is the property the §9/§11 interlock needs, and it holds
whether the new page was extended onto the end of the relation or recycled out of the free space map
(§18). Since §22 the split also puts a downlink for each new page into the parent, in a record of
its own, with the page to its left flagged LION_PAGE_INCOMPLETE_SPLIT in between; and a split of the
ROOT is a push-down that keeps the root at its block. A whole posting set is freed when its entry is
deleted (§18); individual pages of a live one are never unlinked, so a leaf VACUUM has emptied stays
in the list and still owns the range its parent gave it.
INLINE payloads are packed without padding, so containers inside them are unaligned: read them with
`lion_inline_fetch()` into an aligned buffer; never cast into the payload. Containers stored as page
items are MAXALIGNed and may be used in place.

Free space accounting: a container page is "full" for a given container when
PageGetFreeSpace(page) < MAXALIGN(size) + sizeof(ItemIdData). Splits guarantee progress because every
container ≤ 4104 bytes and a fresh page holds at least one.

**Growth slack inside an item** (v1 write wave). An item on a container page may be allotted MORE
bytes than its header needs, so that the next few members can be added inside it. An item therefore
has two sizes, and both are needed: `lion_item_size()` is its LOGICAL size, derived from the header,
and is what every reader uses to find where it ends; `ItemIdGetLength()` is its ALLOCATED length,
which is what it may grow to in place. `lion_item_alloc_size()` chooses the second whenever an
insert writes an item: MAXALIGN(size) plus size/8 clamped into [`LION_ITEM_SLACK_MIN` = 8,
`LION_ITEM_SLACK_MAX` = 64] bytes, capped at LION_CONTAINER_MAX_SIZE, dropped entirely when the page
has no room for it, and never for a BITSET (4104 bytes is already the maximum an item can be). The
slack is a fraction of the item rather than a fixed 64 bytes on purpose: a key whose TIDs are
spread thinly owns dozens of ~50-byte items per page, and 64 bytes of slack each would nearly halve
what a page holds; MAXALIGN padding, which the page spends either way, is part of the slack and
therefore free. Slack bytes are zeroed when the item is written, so that the page image is
deterministic and a GenericXLog delta of a later change stays small. An item that is written back
over itself keeps the allocated length it already has whenever that is still enough (and not
wastefully more), because then PageIndexTupleOverwrite() moves no other item on the page at all;
the exception is an item that has far more room than it can use - a segment replaced by the
container one of its container keys was promoted to - which gives the excess back.

An insert whose item has room inside it adds the member there and nowhere else
(`lion_insert_container_inplace()`, `lion_insert_segment_inplace()` in lion_insert.c): the item keeps
its offset and its allotted length, no other item on the page moves, minckey/maxckey change only
when a segment's range really grew, and the WAL delta is the handful of bytes that changed. A
BITSET always qualifies, an ARRAY needs 2 spare bytes and a cardinality below LION_ARRAY_MAX_CARD, a
RUN needs 4 and a run count below LION_RUN_MAX_NRUNS (both would otherwise turn into a bitset), and
a sparse segment needs 6 and a container key that stays below LION_SPARSE_THRESHOLD members (else
the key is promoted, which is not an in-place change). Otherwise the general path runs - copy out,
mutate, write back, split if needed - and leaves fresh slack behind, which is where the slack of a
growing key comes from in the first place. **Only inserts add slack**: ambuild writes items at
their exact size, because a bulk-built index is read-mostly and the space would be pure loss, and
so does VACUUM (`lion_chain_put_items_locked()` is the no-slack form of
`lion_chain_put_items_locked_ext()`).

Slack is bounded by `LION_ITEM_SLACK_LIMIT` so that it can never be mistaken for corruption:
verify() accepts an item whose allocated length is up to that much above its logical size, and
rejects anything else (including any item above LION_CONTAINER_MAX_SIZE, which is what lets every
reader copy an item out by ItemIdGetLength() into a fixed work buffer). lion_index_stats()
reports the total as `slack_bytes`; `container_bytes` counts logical bytes only, and `free_bytes`
comes from PageGetFreeSpace(), which does not see intra-item slack at all.

**Growth slack inside an INLINE payload** (implemented; §25 left it undone and said what shape it
should take). An INLINE entry's payload is packed, so adding a member to a container inside it
moves every byte above that container - and, before this, the entry itself was rebuilt at its new
length and written back with PageIndexTupleOverwrite(), which moved every OTHER entry on the
directory leaf as well. That is 1.5-2.9 KB of WAL per insert in generic mode, where the record is a
byte diff of the page, and a memmove of the page tail in both modes. An entry an insert rewrites is
therefore allotted a few bytes MORE than its payload needs, by the same rule as an item
(`lion_entry_alloc_size()`: paylen/8 clamped into [LION_ITEM_SLACK_MIN, LION_ITEM_SLACK_MAX]), and
the next insert whose payload still fits inside them keeps the entry's allocated length exactly as
it is: no other entry on the leaf moves, and the record covers this entry alone.

The slack needs no header field, and deliberately does not use the two spare bytes of §24: it is
ZEROED, and `lion_inline_fetch()` already stops at the first zero item header because no item kind
is 0. That is the convention VACUUM's shrink-in-place has used since §18, so the two kinds of slack
meet in one entry with no further rule, and verify() bounds both at LION_ENTRY_SLACK_BOUND and
requires every byte of them to really be zero.

Two caps keep it from changing anything else. Slack may never push the STORED payload past
`inline_limit`, and it is never worth a leaf split: the entry grows only into the free space the
page already has (`PageGetExactFreeSpace()`), else it is written at its exact size and
`lion_dir_place()` splits as it always did. And the SPILL decision is made on the payload the key
really uses, before the allocation is chosen, so slack can never make a posting set spill early -
the test in `lion_insert_inline()` is on `newlen`, not on what the entry is allotted. As for items,
only inserts add it: ambuild writes entries at their exact size and VACUUM keeps the length it
finds. lion_index_stats() reports it as `inline_slack_bytes`, apart from `slack_bytes`, because it
is an entry's and not an item's.

Measured (release, 1M rows, 10,000 single-row inserts into one index, §25's harness): the c20k
column falls from 2918.7 to 173.0 bytes of WAL per insert in generic mode and from 422.4 to 180.5
in rmgr mode, and c1m from 1682.2 to 106.8 and from 124.1 to 82.0. The 8-index portfolio insert
falls from 53.53 to 9.48 MiB (generic) and from 29.21 to 9.08 MiB (rmgr) in steady state, and the
index is the same size afterwards to the byte - the slack lands in space the leaves already had.

Page allocation: `lion_alloc_page()` takes a page - a recycled one from the free space map when one
is safe to take, else a fresh block from ExtendBufferedRel - and `lion_wal_init_buffer()`
initialises it and registers it in the caller's record, so a crash cannot leave an initialised page
that nothing links to; `lion_new_buffer()` is the same in a record of its own. The two are separate
calls since §25: taking the page is the fallible half and has to happen before the record opens,
because an rmgr-mode record is written inside a critical section. The recycling rule, what `heaprel` is for and
why a chain's head page never comes from the map are in §18. verify() reports DELETED pages as the
ordinary free pages they are, unreferenced never-initialised or empty pages as WARNINGs (the next
VACUUM's sweep turns them into free pages) and anything else as an ERROR.

## 5. Locking protocol (deliberately coarse in v0)

**Where this section says "bucket head page", read "the directory LEAF that holds the entry" (§21).**
The directory is a B-tree now, so step 1 of INSERT is a descent rather than a modulo, step 2's walk
of a bucket chain is a binary search plus a scan of the prefix run, the bucket-directory guard of
step 2 is gone entirely (the tree grows by splitting), and ambuild sizes nothing. Everything else -
the lock ordering, the one-record-per-atomic-step rule, the measured concurrency ceiling and why
shortening the hold would not move it - is unchanged, and the leaf serialises the writers of a key
exactly as the bucket head page did.

**A MULTICOLUMN index changes none of it (§24).** One relation holds every key column's entries in
one directory, so one aminsert is n independent single-key inserts - one per column, each taking and
releasing the leaf that holds ITS entry - and two columns of one index contend exactly as two
single-column indexes would, except that they may land on the same leaf. The lock ordering below is
unchanged because a directory leaf is a directory leaf whatever column's entry is on it.

Lock ordering: directory pages (root to leaf) → container pages (left to right) → new page.
Never lock a page to the left of one you hold. The meta page is read once at relation open and
cached (and re-read when a root split invalidates the cached root, §21).

INSERT (`lion_insert.c`)
1. Hash the key; lock the bucket head page EXCLUSIVE and hold it until the insert is complete.
2. Walk the bucket chain (lock each further bucket page EXCLUSIVE while inspecting/modifying it;
   pages other than the head may be released when done) to find the entry. If absent, add an INLINE
   entry with one 1-member array container (splitting the bucket chain by appending a new bucket page
   if no bucket page has room).
   **Bucket directory guard** (v1 write wave): the walk counts the pages it visits, and a chain
   longer than `LION_BUCKET_PAGES_WARN` = 4 pages means the bucket holds about six times the entry
   bytes ambuild sizes a bucket for (three quarters of a page), i.e. the index has outgrown the
   directory it was built with. The threshold is not the "two pages per bucket on average" this
   policy is stated as, because what an insert can observe cheaply is one bucket's chain - a
   maximum, not an average - and hash skew plus ambuild's byte estimate (~30% low for keys whose
   TIDs spread thinly, §13) leave a correctly sized index with three-page buckets; measured on the
   20000-key column of bench/write_micro.sh's portfolio, which warned at a threshold of 2 right
   after a clean build. An index that has really outgrown its directory is far past four pages:
   100k keys in an index created empty give 64 buckets of twelve pages each.
   The backend then says so once per index (`lion_warn_bucket_chain()`), suggesting a REINDEX. It is
   advisory in exactly the way the §17 cardinality guard is - the index keeps working and keeps
   taking rows - and the estimate is one bucket's chain rather than an average over the directory,
   because hashes spread entries evenly enough and walking the whole directory on every insert would
   cost more than the warning is worth. An index whose `buckets` reloption was set explicitly is
   never warned about: that count is what its owner asked for. lion_index_stats() reports the
   same quantity exactly, as `max_bucket_pages` (the longest chain in the index).
   This is a warning and not online growth on purpose. Growing the directory in place would mean
   splitting buckets (a new bucket count changes `lion_bucket_of()` for every key, so either the
   whole directory is rehashed under a lock that stops every reader, or the index keeps a split
   point and two hash functions, as dynamic hashing does - and then every reader, the count
   pushdown and VACUUM have to consult it, and a crash in the middle has to leave the two halves
   consistent). Measured: 100k keys inserted into an index created empty keep 64 buckets and 773
   bucket pages, and the 100-key IN median goes from 0.104 ms (bulk-built, 847 buckets) to 0.321 ms
   (bench/results/2026-09-21-stress). A REINDEX costs one build and puts it back; online growth is
   out of scope for v1.
3. INLINE: rebuild the inline payload in a work buffer with the member added; if ≤ inline_limit and it
   fits on the bucket page (after PageRepairFragmentation if needed), overwrite the entry tuple;
   else spill to a chain (allocate one page, add all containers) and fall through to CHAIN.
4. CHAIN: find the page for ckey; lock it EXCLUSIVE; copy container out, add member (or create a new
   1-member container), write back / split as in §4; update entry (ncontainers, ntids, tail).
   Fast path: when the item already on the page can take the member without changing the number of
   bytes the page has allotted it, the member is added directly in the GenericXLog page image
   (same locks, same single record carrying the entry tuple) instead of being copied out, mutated
   and written back with PageIndexTupleOverwrite. Nothing else on the page changes: same allotted
   length, same offset, same ckey, so min/max stay put (a sparse segment may extend its range, and
   then only min/max move). A BITSET always qualifies - it is 4104 bytes whatever its cardinality -
   and an ARRAY, a RUN or a segment qualifies when the item has growth slack (§4), which the general
   path leaves behind whenever an insert writes an item.
   The page is located with the exclusive lock the insert needs anyway: the tail page is tried
   first (`lion_insert_lock_chain_page()`), because that is where every insert into a growing posting
   set lands, and only a ckey the tail does not own falls back to `lion_chain_find_page()`, which
   walks from the head. That saves the SHARE acquisition lion_chain_find_page() would take on the
   very same tail page just to read its minckey: two lock acquisitions per appending insert instead
   of three, all of them inside the bucket-lock window. It did NOT move the concurrent-insert
   ceiling of step 6 (643 tps either way, measured by swapping the two builds in one session), which
   is what step 6 explains; it is kept because it strictly removes work from inside that window, and
   an insert whose ckey is not on the tail pays the same number of acquisitions as before (an
   exclusive lock on the tail where it used to be a shared one).
5. All page modifications go through GenericXLog: one GenericXLogStart per atomic step, registering
   at most 4 buffers (a split touches P, N, possibly M, and the bucket page).
6. Release everything. Inserts to different buckets do not block each other; inserts to the same
   bucket serialize. Documented and accepted for v0, and measured for v1 (bench/write_micro.sh,
   8 clients × 100 rows per transaction, fsync on, synchronous_commit on, 1 index):

       transactions/s, 100 rows each      1 client   4 clients   8 clients
       roaring, 2 key values                 387        629         643
       roaring, 1000 key values              371       1078        1890
       btree, 2 key values                   460       1128        1466
       no index, 2 key values                511       1170        2296

   With a thousand key values the index side all but disappears (5.1× scaling, 82% of the no-index
   ceiling); with two it flatlines at 1.7×. The wait-event profile of the two-value case is 57%
   `Buffer/BufferExclusive` against btree's 31%, so the buffer content locks of the two keys' pages
   are indeed where the time goes - but shortening the hold is NOT what would fix it, and the
   numbers say why. A single-client roaring insert costs ~7.7 µs of which the serialized part is the
   GenericXLog record: registering a buffer copies the whole 8 KB page, GenericXLogFinish() diffs
   it against the copy and applies the image, so a record covering the container page and the
   bucket page moves ~48 KB of memory per single-TID insert, all of it inside both locks because
   §5 requires the entry's counters to travel with the container change. Repeating the burst on an
   UNLOGGED table - no delta, no XLogInsert - lifts it only from 643 to 868 tps (btree 1466 → 2150,
   no index 2296 → 19538), so the cost is the page-sized copying rather than the logging.
   The protocol that would shorten the bucket hold (release the bucket page, walk and lock the
   chain page, then ConditionalLockBuffer() the bucket page and re-read the entry, retrying from
   the top on failure, as ambulkdelete does in §11) was therefore NOT implemented: it does not
   remove the record write from the window where both pages are held, and it adds an acquisition
   and a retry path. It is worth doing for the case it really helps - an insert whose ckey is in
   the MIDDLE of a long chain, where `lion_chain_find_page()` walks many pages under the bucket
   lock - and that is the shape to revisit it in. The structural fix for hot-key inserts is a
   custom WAL resource manager (RegisterCustomRmgr, PG 15+) with physical records ("set member m of
   the item at offset o", "add n to the entry's ntids") instead of GenericXLog's page diffs, which
   would cut the critical section by an order of magnitude.

SCAN (`lion_scan.c`, amgetbitmap)
1. Lock bucket head SHARED, walk the bucket chain (lock coupling not required for bucket pages:
   hold one page at a time; entries are only ever appended to bucket pages or overwritten in place).
2. On finding the entry: INLINE → copy the payload out, release, emit. CHAIN → copy head, release the
   bucket page, then descend to the leftmost LEAF (§22: head is the posting tree's root) and walk the
   leaves holding one SHARED page lock at a time: read all items and the rightlink under the lock,
   release, lock the rightlink. Splits only move items to a new page immediately to the right and
   require an EXCLUSIVE lock on the source, so a reader sees every container exactly once (it read
   the items and the rightlink atomically). The descent hands the first leaf back LOCKED, which is
   what keeps a root push-down from slipping in between.
3. Emit each container into the TIDBitmap: for each of the ≤ 64 heap blocks the container covers,
   collect its offsets into an ItemPointerData array and call tbm_add_tuples once per heap block
   (never one call per TID). Return the number of TIDs.
4. amgetbitmap with a NULL scan key (`col = NULL`) or a key of the wrong type returns 0 TIDs.
   SK_SEARCHNULL emits the NULL entry and SK_SEARCHNOTNULL every other entry (§14); SK_SEARCHARRAY
   emits one entry per non-NULL element of the array (§15).
5. The index has one key column, but the planner may hand the scan more than one qual on it
   (`b = ANY (x) AND b = ANY (y)`, an equality next to a null test). The scan answers the most
   selective-looking one - a plain equality, else a list, else a null test - and passes recheck =
   true to tbm_add_tuples() for every TID, so the bitmap heap scan re-applies the original quals.
   Emitting a superset with recheck set is correct; silently dropping the other quals would not be.

VACUUM (`lion_vacuum.c`, ambulkdelete)
1. For each bucket: two passes as described in §11 (the head is cleanup-locked only while its own
   INLINE entries are modified; chain pages are processed without holding the head across waits).
2. For each entry: INLINE → filter the payload through the callback and repack. Note that removal can
   GROW a container (every-other-member deletion turns a RUN into a 4104-byte BITSET), so a filtered
   INLINE payload may exceed inline_limit or the page: then the entry spills to a chain during VACUUM.
   The bound is lion_inline_max(), as for INSERT and ambuild: next to a ~2000-byte key a payload
   inside inline_limit can still overflow LION_MAX_ENTRY_SIZE (an ARRAY that a 2030-member deletion
   made out of a RUN, say), and VACUUM tested inline_limit alone and failed on it every run until
   the 2026-09-23 review.
   CHAIN → walk the posting tree's leaves, left to right from the leftmost one; each leaf is locked
   with LockBufferForCleanup (this is the
   interlock that phase 2 relies on: a heap-skipping reader keeps the page pinned while it consults
   the visibility map). Filter every container with lion_container_remove_if, run
   lion_container_optimize, write back in place or, if it grew and no longer fits, re-place it through
   the chain machinery (which may split the page); delete empty containers with one
   PageIndexMultiDelete per page (it compacts; no PageRepairFragmentation needed); update min/max.
   Empty pages stay in the chain. Every page record also carries the updated entry (ncontainers/ntids),
   so a crash cannot desynchronise the counters. An entry whose ntids reaches 0 is DELETED, and its
   chain freed, in a final step for its bucket page (§18).
3. Report stats: num_pages, num_index_tuples = Σ ntids, tuples_removed, and the pages this cycle
   freed (pages_newly_deleted/pages_deleted/pages_free, §18).
4. amvacuumcleanup: if stats is NULL (no bulkdelete was needed) return a fresh stats struct by
   counting pages; otherwise pass it through.

BUILD (`lion_build.c`)
1. table_index_build_scan callback pushes (hash int4, key datum, code int8) into a tuplesort created
   with tuplesort_begin_heap over a 3-attribute TupleDesc, sort keys (hash ASC via int4 btree,
   code ASC via int8 btree), TUPLESORT_RANDOMACCESS, maintenance_work_mem. A NULL key goes in with
   hash 0 and the key column NULL (§14); the two passes below group by the isnull flag first.
2. Pass 1 over the sorted data counts distinct keys (equal hash ⇒ compare with the equality proc,
   remembering the small set of distinct keys seen for the current hash value) and adds up the BYTES
   their entry tuples will need: for each key, MAXALIGN(MAXALIGN(LION_ENTRY_HDRSZ + keylen) +
   min(payload, inline_limit)) + sizeof(ItemIdData), where the payload of a key with n members is
   estimated as 6n bytes below LION_SPARSE_THRESHOLD members (one sparse pair each, §13) and
   LION_CONTAINER_HDRSZ + 2n from there on (an ARRAY container). Both halves are rough - pass 1 does
   not group the codes by container key - but they have the right order of magnitude at both
   extremes. nbuckets = reloption if set, else ceil(total bytes / (BLCKSZ * 3/4)), clamped to
   [LION_DEFAULT_BUCKETS = 64, 65536]. Bytes rather than key counts, because every bucket owns a head
   page whether it needs one or not: sizing by distinct keys spent 32768 pages (256 MB) on a
   1M-key index whose entries were 84 MB. The floor matters because indexes are usually built on
   empty tables and filled later.
   The byte total is scaled up by `pg_class.reltuples / indexed rows` when the heap is known to hold
   more rows than this build put in the index, because the directory is sized once and nothing ever
   resizes it (INSERT step 2 warns when it has been outgrown). Only ever up, and only for an index
   over the whole table: a partial index holds the rows its predicate selects, and sizing that for
   the whole heap would spend pages on buckets that stay empty. reltuples is -1 until something
   analyses the heap, so an index built on an EMPTY table still gets the floor - for that case the
   only way to say how large the index is going to be is the `buckets` reloption, which means an
   exact count (§4).
3. tuplesort_rescan; pass 2 groups by key. Because codes are sorted within a hash, and keys sharing a
   hash are rare, keep one open builder per distinct key of the current hash (a builder = ordered
   list of containers under construction; use lion_container_append_sorted with a per-key "last ckey"
   and finish each container with lion_container_optimize when the ckey changes). When the hash
   changes, flush all builders: small payload → INLINE entry; else allocate container pages and fill
   them sequentially (fill each page until the next container does not fit; set rightlink,
   min/max), then add the CHAIN entry.
4. Pages are written through the bulk-write API (storage/bulk_write.h), which is what the nbtree
   and GiST builds use: `smgr_bulk_start_rel()` once for MAIN_FORKNUM, `smgr_bulk_get_buf()` for
   each page image, `smgr_bulk_write()` when that page is final, `smgr_bulk_finish()` at the end.
   Pages go straight to the file without passing through shared buffers, are WAL-logged in batches
   of up to 32 as full-page images (or not logged at all for an unlogged relation or under
   wal_level = minimal, in which case the relation is registered for the next sync), and each page
   is written EXACTLY ONCE. Nothing may touch these blocks through the buffer manager until
   smgr_bulk_finish() has returned.
   Writing each page once is the point: the old route through the buffer manager logged one
   GenericXLog record per entry tuple, so a build of 632k keys paid 632k page diffs and 632k WAL
   records to fill 15k pages (measured, 1M rows, one column: 4.7 s and 76 MiB of WAL against 1.9 s
   and 39 MiB now; the eight-index portfolio of bench/COMPARISON.md went 13.0 s / 95 MiB to
   11.3 s / 57 MiB, which is below btree's 59 MiB).
   It also means a page has to be FINAL before it is written, and a bucket page is only final when
   the last entry that hashes to it has been added. Bucket pages therefore live in backend-local
   memory - one 8 KB image per bucket page that holds entries, allocated lazily - until pass 2 is
   over, and are written afterwards: head pages in bucket order, then the overflow pages in block
   order. That is the cost of this route: the directory is sized at three quarters of a page per
   bucket, so the images come to ~1.3× the bytes the entries need, bounded by LION_MAX_BUCKETS pages
   (512 MB) and by nothing else. Container pages are final as soon as the next container does not
   fit, so only one per open key exists at a time.
   Block numbers come from a counter rather than from extending the relation, in the same order the
   buffer-manager route extended it (meta = 0, bucket directory = 1 .. nbuckets, then container
   pages and further bucket pages on demand), so an index built by either route has the same page at
   the same block - which is what keeps every regression output identical. The directory is a hole
   in the file while pass 2 runs; the bulk writer fills a hole with zero pages when a later block is
   written past it, and the real pages overwrite them afterwards, which costs one extra 8 KB
   buffered write per bucket page and no WAL.
5. ambuildempty: init meta + bucket pages (nbuckets = reloption or 64) in INIT_FORKNUM with
   log_newpage, as contrib/bloom does.

## 6. Handler settings (`lion_am.c`)

    amstrategies = 5 (1 equality, 2 @>, 3 &&, 4 <@, 5 @@ -- see §17)
    amsupport = 4 (1 = hash function, same as hash AM; 2 and 3 = GIN's extraction procs, §17;
                   4 = btree comparison of the KEY type, optional, §21)
    amoptsprocnum = 0                amcanorder = false        amcanorderbyop = false
    amcanhash = false                amconsistentequality = true   amconsistentordering = false
    amcanbackward = false            amcanunique = false       amcanmulticol = false
    amoptionalkey = false            amsearcharray = true      amsearchnulls = true
    amstorage = true (§17)           amclusterable = false     ampredlocks = false
    amcanparallel = false            amcanbuildparallel = false    amcaninclude = false
    amusemaintenanceworkmem = true   amsummarizing = false     amkeytype = InvalidOid
    amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL (§11, §18; cleanup stays with the leader)
    amgettuple = liongettuple (§29)  amgetbitmap = liongetbitmap    amcanreturn = NULL
    ammarkpos/amrestrpos = NULL      parallel scan callbacks = NULL
    amcostestimate: genericcostestimate() then indexCorrelation = 0 (as contrib/bloom)
    amoptions: reloptions `fillfactor` (int, 10..100, default 90, §21), `inline_limit` (int bytes,
    64..4096, default 4096), `max_entries` (int, 0 = unlimited, §17) and `buckets`, which is
    accepted and ignored with a NOTICE since format 4 (§21), via add_reloption_kind /
    add_int_reloption / build_reloptions.
    ambuild: refuses a table whose table AM is not the heap, by routine (§2).
    amvalidate: a scalar opclass must have support proc 1 with signature (T) → int4 and operator
    strategy 1; a multi-key one (§17) procs 2 and 3 and strategies within {2,3,4,5}; proc 4 is
    optional for both and is the only support function allowed to be cross-type (§21).
    Handler follows contrib/bloom in master: `static const IndexAmRoutine amroutine = {...}` returned
    with PG_RETURN_POINTER.

Operator classes (in `pg_lion--0.1.sql`): one DEFAULT opclass per type, reusing the hash AM's
support-1 functions and the btree AM's support-1 functions as proc 4 (§21), plus the two multi-key
classes of §17. Generate the list from the dev cluster with
`SELECT ... FROM pg_amproc JOIN pg_opclass ... WHERE amname='hash' AND amprocnum=1` for at least:
int2, int4, int8, oid, bool, "char", text, varchar (via text), bpchar, bytea, uuid, date, time,
timestamp, timestamptz, interval, numeric, float4, float8, macaddr, inet, name, jsonb, enum types
via anyenum (hashenum). Strategy 1 operator = the type's `=`.

## 7. SQL functions (`lion_funcs.c`)

    lion_index_stats(regclass, OUT attno int2, OUT directory_height int, OUT leaf_pages bigint,
        OUT internal_pages bigint, OUT ordered bool, OUT entries bigint,
        OUT inline_entries bigint, OUT container_pages bigint, OUT containers bigint,
        OUT array_containers bigint, OUT bitset_containers bigint, OUT run_containers bigint,
        OUT ntids bigint, OUT container_bytes bigint, OUT free_bytes bigint,
        OUT sparse_segments bigint, OUT sparse_members bigint, OUT null_tids bigint,
        OUT empty_tids bigint, OUT slack_bytes bigint,
        OUT deleted_pages bigint,
        OUT posting_internal_pages bigint, OUT max_posting_height int) RETURNS SETOF record
        -- ONE ROW PER KEY COLUMN (§24), in attno order.  Counters that describe an entry or a
        -- posting set - entries, inline_entries, ntids, null_tids, empty_tids, the container and
        -- sparse counts, container_bytes, slack_bytes, container_pages, posting_internal_pages -
        -- are that column's own; counters that describe the RELATION - directory_height,
        -- leaf_pages, internal_pages, free_bytes, deleted_pages, max_posting_height - are repeated
        -- on every row, because one directory leaf holds whatever columns' entries land on it.
        -- A container page IS attributed to its column, through the owner_head stamp of §18 and
        -- the attno of the entry that owns that head.
        -- `ordered` is per column, and false means that column's key type has no btree opclass, so
        -- its entries are in a complete but arbitrary order;
        -- container counts include INLINE containers; free_bytes sums directory and container pages;
        -- null_tids is the member count of the reserved NULL entry (§14) and empty_tids that of
        -- the reserved no-key entry (§17); container_pages counts posting-tree LEAVES and
        -- posting_internal_pages the pages above them, with max_posting_height the tallest
        -- posting tree in the index (§22; 0 means every set fits one page)
    lion_index_posting_root(regclass, key anyelement) RETURNS bigint
        -- the ROOT block of one key's posting tree, NULL when the key has no entry or its set is
        -- still INLINE.  For tests only: §22 requires the root block never to move, because it is
        -- the identity every page of the set is stamped with (§18).
    lion_index_verify(regclass, heapallindexed bool DEFAULT false) RETURNS void
        -- ERRORs on any structural inconsistency: the key column of every entry (in range, and
        -- never below the column of the entry before it, §24), at most one reserved NULL and one
        -- reserved EMPTY entry PER COLUMN, page ids/flags, meta values, entry flags,
        -- ascending ckeys within pages and across rightlinks, min/max correctness, container_check
        -- on every container, ntids/ncontainers sums; with heapallindexed, scans the heap with a
        -- fresh snapshot and checks that every visible tuple's TID is present under its key in
        -- EVERY key column (§24) - refusing (lion_index_usable(), §9) when this transaction's
        -- snapshot may not use the index.
    (phase 2) lion_index_count(regclass, key anyelement) RETURNS bigint

## 8. Module ownership

    src/lion_tid.h            TID ↔ code helpers                          (fixed; written by the architect)
    src/lion_container.h/.c   container library + test/unit/container_test.c   (agent "container")
    src/lion.h                on-disk structs, LionState, prototypes of lion_pages.c (skeleton by architect)
    src/lion_pages.c          meta/bucket/entry/chain primitives, splits, page alloc   (agent "am-core")
    src/lion_am.c             handler, options, validate, costestimate, buildempty     (agent "am-core")
    src/lion_build.c          ambuild                                                   (agent "am-core")
    src/lion_scan.c           ambeginscan/rescan/endscan/getbitmap                      (agent "am-core")
    src/lion_insert.c         aminsert                                                  (wave 2)
    src/lion_vacuum.c         ambulkdelete/amvacuumcleanup                              (wave 2)
    src/lion_funcs.c          stats/verify (+ count in phase 2)                         (wave 2)
    src/lion_multikey.c       GIN-style extraction and query trees (§17)                (wave 3)
    src/lion_ordered.c        LionOrdered: lion-filtered, btree-ordered scans (§30)
    pg_lion.control, pg_lion--0.1.sql, Makefile, test/                    (am-core, then wave 2)

Coding conventions: PostgreSQL C style (tabs, K&R braces on their own line for functions, /* */
comments, `elog(ERROR, ...)` for internal errors, `ereport` with errcode for user-facing errors),
compile clean with `-Wall -Wextra -Wno-unused-parameter` and cassert enabled. No CRoaring dependency.

## 9. Phase 2a: heap-skipping count with a visibility-map interlock (`lion_count.c`)

Goal: `count(*) WHERE k1 = v1 [AND k2 = v2 ...]` computed from containers, visiting the heap only for
pages that are not all-visible, with exactly the MVCC semantics of the equivalent SELECT under the
caller's snapshot.

Why this is safe (the argument reviewers will check — keep the code shaped like it)
- A TID that is in the index but whose tuple is dead can be counted wrongly only if the heap page is
  all-visible in the VM at the moment we check. VACUUM sets all-visible only after it has removed the
  dead TIDs from *every* index (ambulkdelete) and then from the heap page.
  (Since PostgreSQL 19 on-access pruning sets all-visible too, so VACUUM is one of two writers.
  What the argument really rests on is narrower and holds for both: a page holding a dead root
  line pointer - LP_DEAD - is never marked all-visible by anyone, and only VACUUM, after
  ambulkdelete, turns LP_DEAD into LP_UNUSED. §11's last subsection proves it against 19's
  pruneheap.c.)
- Our ambulkdelete takes `LockBufferForCleanup` on every container page and on every bucket page
  whose INLINE entries it rewrites. Cleanup locks wait for all pins to drop.
- Therefore: while the counting code holds a *pin* on the page it took a container from, VACUUM
  cannot have completed ambulkdelete for that page, so it cannot have set all-visible on any heap page
  whose dead TIDs are still in that container. Rule: **check the VM for a container's heap blocks
  before releasing the pin on the page the container came from.** The content lock may be dropped
  (copy the containers out under SHARE lock, keep the pin, consult the VM, then unpin).
- Tuples inserted after our snapshot: heap_insert clears the VM bit, so their pages are not
  all-visible and go through the heap recheck, which applies the snapshot. Tuples deleted but still
  visible to us: the page is not all-visible (a deleted tuple is not visible to all); recheck sees it.
- `VM_ALL_VISIBLE` may return a slightly stale true only if the bit was cleared concurrently by an
  insert/update/delete of a tuple that cannot be visible to our snapshot anyway (same reasoning as
  index-only scans; see visibilitymap_get_status comments).
- Serializable isolation: call `PredicateLockPage(heap, blk, snapshot)` for every heap block counted
  from the VM without a heap visit, exactly as heapam_indexscan.c does for index-only scans.

Four rules added after the 2026-09-20 adversarial review (all implemented and regression-tested in
test/sql/security.sql and test/isolation/count_serializable.spec):
- **Privileges.** The SQL count functions require what the equivalent query requires: SELECT on the
  table, or SELECT on every column the index reads - its key columns, the columns of its
  expressions and of its PREDICATE (a partial index's count is `... WHERE pred AND col = key`, so
  SELECT(id) alone on `(id) WHERE secret` would reveal `secret` a row at a time; a whole-row
  reference needs the table-level grant; an index that reads no column at all, one on a constant,
  needs SELECT on at least one column, as `SELECT count(*) FROM t` does); otherwise `permission
  denied` (2026-09-23 review). EXECUTE is required on every function the index's expressions and
  predicate call, read from pg_index as stored - the relcache's copies are planner-simplified,
  with inlinable SQL functions already replaced by their bodies - whatever the table grants. The
  CustomScan path is covered by the executor's own ExecCheckPermissions on the range table, and a
  partial index reaches it only when the query implies the predicate, i.e. references it.
  **EXECUTE on what a count replaces** (2026-09-23 review: a pushed-down `count(*) WHERE k = 1`
  answered after `REVOKE EXECUTE ON FUNCTION count()` or `int4eq` from PUBLIC, and so did the SQL
  functions). The rule is the one above - ask exactly what the replaced plan asks, no more, no
  less - and what core asks was read from nodeAgg.c/execExpr.c and verified against the ordinary
  plan on 16 and 20: ExecInitAgg checks EXECUTE on each aggregate for the current user and on its
  transition and final functions for the AGGREGATE'S OWNER; ExecInitFunc checks every function
  and operator of an initialised expression - the scan's quals, including a hashed IN list's hash
  function and a hash or nested-loop join's clause - for the current user; the grouping equality of
  a GROUP BY is checked (ExecBuildGroupingEqual), its hash and sort support functions are not, and
  `count(DISTINCT k)` compares k through an unchecked FmgrInfo, so it runs with `int4eq` revoked.
  Each check fires InvokeFunctionExecuteHook. So the planner records, in custom_private member
  `LION_PRIV_EXECUTE` (shape 10), the aggregates of the target list and the HAVING (setrefs.c
  turns the HAVING's into references to the node's own columns, so core never sees them), the
  functions of every WHERE clause - OR trees and IN lists included, from the parent's quals for a
  partitioned table - and of an FK-side join's clause (§27), and the GROUP BY's equality
  functions; `lion_begin_custom_scan()` checks them for GetUserId() with core's errors
  (`permission denied for function int4eq`, `... for aggregate count`) before it opens anything,
  every time the plan starts, so a cached plan answers a REVOKE or a SET ROLE as the ordinary one
  does. Plain EXPLAIN initialises the plan too and core then checks the quals and the aggregates
  but not a HashAggregate's grouping equality (its hash table is not built), so the node skips
  that one under EXEC_FLAG_EXPLAIN_ONLY. One deliberate difference: a merge join compares through
  the btree support function unchecked, so a join the planner would have merged runs with its
  operator revoked while the node refuses it - the node keeps the SQL meaning, the query calls `=`.
  The SQL functions stand for `SELECT count(*) FROM t WHERE col = key` (or `= ANY (keys)`, or
  `GROUP BY col`), so they require EXECUTE on count() - checked as an aggregate, as above - and on
  the equality function they look up with: strategy 1 of the key column's opfamily for (opcintype,
  the key's type as "SQL surface" below resolves it), which is `int48eq` for an int8 key on an int4
  column exactly as in the query,
  and (opcintype, opcintype) for the grouped form. These checks follow the exact SELECT check,
  under the lock (test/sql/security_exec.sql). One case follows core rather than the rule:
  a clause implied by a PARTIAL index's predicate is dropped by the planner, so a keyless plain
  or index-only scan of that index answers without calling the clause's functions, and without
  checking EXECUTE on them. Whether the clause is implied can depend on who built the index's
  relcache entry (a role without EXECUTE sees an inlinable SQL function un-inlined, which then
  matches the query's clause exactly). A B-tree behaves identically; the predicate was evaluated
  by whoever built the index, as core intends. The
  diagnostic functions check neither privileges nor RLS - `lion_index_posting_root()` answers key
  membership, `lion_index_stats()` gives counts, `lion_index_verify()` reads the heap,
  `lion_index_wal_mode()` locks whatever it is handed - so they are revoked from PUBLIC like
  pageinspect's and amcheck's (`lion_index_stats()` is granted to `pg_stat_scan_tables`, as
  pgstattuple's functions are). `lion_index_verify()` names a missing row's key value only to a
  superuser; a role it is granted to gets the TID (test/sql/hardening.sql). The cheap half of the
  count functions' check - SELECT on the table or on at least one column - comes BEFORE any lock
  is taken, so a role with no privilege cannot queue behind (and hold up) locks on a table it
  cannot read; the exact check follows under the lock, where the index definition can be trusted
  (third round of the 2026-09-23 review, test/isolation/count_lock_privilege.spec).
- **Install scripts.** Every support function a script names is found in pg_catalog or, for
  citext, through `@extschema:citext@` - never in the target schema, where a role with CREATE could
  plant one that then runs as whoever inserts into the index (test/sql/hardening.sql).
- **Row-level security.** The SQL functions refuse a table on which RLS applies to the caller
  (policies would have to be evaluated per row); the CustomScan declines relations with security
  quals, so the ordinary plan applies the policies.
- **Table access method.** Everything here reads the heap's visibility map; the SQL functions
  refuse, and the CustomScan declines, a table whose table AM is not the heap, which ambuild has
  already refused to index (§2).
- **SERIALIZABLE.** index_beginscan() takes a relation-level predicate lock on any index whose AM
  has no ampredlocks; the count reads indexes without a scan, so it takes PredicateLockRelation on
  every index it opens, before any lookup, so that absent keys are covered. Without it two
  serializable transactions could each count an absent key, insert it, and both commit.
- **Hot standby.** The pin interlock relies on ambulkdelete taking cleanup locks, and what replay
  takes depends on which resource manager wrote the record (§25).
  *Generic WAL*: replay takes only exclusive locks, so on a standby a reader's pin does not stop
  replay from removing TIDs and the following heap records from setting all-visible. In recovery a
  count over such an index treats every heap block as not all-visible and rechecks every candidate
  TID (still correct, no longer O(1) per container).
  *The custom resource manager* (§25, implemented): every record that removes a TID or deletes an
  item declares the page it removes them from, and `lion_redo()` takes that block with
  `XLogReadBufferForRedoExtended(..., get_cleanup_lock = true)`, the way `btree_xlog_vacuum` does.
  The argument above then reads on the standby word for word with "VACUUM" replaced by "the startup
  process": while the count holds a pin on the page a container came from, replay cannot have
  removed a TID from that page, so it cannot have replayed the heap record that set any of that
  container's heap pages all-visible. The count uses the visibility map there exactly as on a
  primary, and the standby reader is paid for the way a primary reader is - replay WAITS, which
  `max_standby_streaming_delay` turns into a recovery conflict rather than a wrong answer.
  *That sentence is only half of it* (2026-09-22 review): the TIDs a reader copied can MOVE - a
  split to the right sibling, a root push-down to a new child (§22), an INLINE spill off the
  directory leaf - and be removed from the page they moved to, which the reader does not pin. On
  the primary that removal waits anyway, because VACUUM cleanup-locks every page in chain order
  (§11), but a page VACUUM locks and leaves unchanged writes no record, so replay used to take no
  lock on it at all. VACUUM therefore hands every such page to the next record that takes a
  cleanup lock at redo, as a BARRIER that redo cleanup-locks first (§25, `xl_lion_visit`), and
  the argument holds on the standby for moved TIDs as well. `test/recovery/run.sh` phase 3 parks a
  standby reader with its page pinned and has the primary split, push down or spill that page and
  then VACUUM: before the barrier all three answered the dead rows (50000 for 48548, 1000 for 500,
  200 for 100); with it replay blocks on the reader's pin and every answer is right.
  The decision is per COUNT and not per index (`lion_sources_all_rmgr()`): a container of an
  intersection carries the dead TIDs of every source it came from, so ONE generic-mode source puts
  the whole count back on rechecking everything.

Algorithm `lion_count_keys(Relation heap, int nkeys, Relation *indexes, Datum *keys, Snapshot snap)`
1. For each (index, key): locate the entry (bucket head SHARE lock; copy the entry header; for INLINE
   entries copy the payload while holding the pin; for CHAIN entries note head). Pins are
   budgeted: an INLINE set located past the budget holds none, and §15's "The pin budget" says
   how the count keeps this section's rule for it.
2. Merge-iterate the k posting sets by ckey (they are sorted). For the AND of k containers with the
   same ckey use lion_container_and into a work buffer (k-1 times). Containers whose ckey is missing
   from any set contribute nothing.
   Pin discipline: at any time hold at most one pinned page per index (the page whose containers you
   are currently consuming). Advance a set's cursor page only after its containers have been fully
   consumed, and consume means: for the resulting AND container, do the VM checks (step 3) before any
   source page pin is released.
3. For the result container: for each of the ≤ LION_BLOCKS_PER_CONTAINER heap blocks with members
   (compute member counts per block via lion_container_range_cardinality over the block's lo range;
   skip blocks with 0), `visibilitymap_get_status(heap, blk, &vmbuf)`; if all-visible: count +=
   members, PredicateLockPage; else: append the block's TIDs (lion_code_to_tid) to a recheck list.
4. After the merge completes, recheck: sort the TID list (it is already in TID order if produced in
   ckey order), and for each TID call `table_index_fetch_tuple(fetch, &tid, snap, slot, &call_again,
   &all_dead)` in a loop over call_again (HOT chains); count each visible tuple found. Use
   `table_index_fetch_begin/end`. Tuples for which the fetch finds nothing are not counted.
   (In PostgreSQL 20devel that API is gone and `table_fetch_tid()` replaces it; the heap AM is served
   by one `heap_hot_search_buffer()` loop per heap BLOCK instead, under one share lock.)
5. Return the count. Memory: the recheck list lives in a per-call context and is BOUNDED - it holds
   at most `work_mem` worth of TIDs (floor: a few thousand). When it fills up, the batch it holds is
   rechecked right there, its visible rows are added to the running total and the list starts over;
   the flush waits for a heap-block boundary, so no block is pinned twice and `blocks_rechecked`
   stays exact. Without a bound, 20M candidates - which a standby produces for any 20M-row posting
   set, since it never trusts the VM - meant a 192 MiB array, and 134M candidates an impossible
   allocation.
   Flushing mid-merge does not weaken the interlock above: the ordering rule is that the VM question
   about a container's blocks is asked before the pin on the page that container came from is
   released, and a flush asks no VM question - those are what put the TIDs on the list. Rechecking a
   TID needs no index pin at all: if VACUUM removed that TID meanwhile, the tuple was dead to every
   snapshot including ours (nothing to count, and nothing is found); if the line pointer was reused,
   the new tuple's xmin is later than our snapshot and is invisible to it.
6. Per-query visibility cache (`LionVisCache`, lion_count.h). A grouped count asks the recheck about
   the same dirty heap pages once per group: at 5M rows with 200 groups and 5% of the rows updated,
   9,062 dirty pages cost 415,884 heap block visits (§12's grouped-count finding). The answer cannot
   change between those visits, so it is resolved once per (page, snapshot) and reused. The cache
   maps a heap block to a bitmap of MaxHeapTuplesPerPage bits - which of its ROOT line pointers hold
   a tuple visible to this snapshot - in a hash table in a per-node memory context, bounded by
   `work_mem` in entries; when the budget is reached nothing more is inserted and the blocks that
   did not get in are fetched per batch exactly as before (`cache_full` counts those visits).
   - Where: in the flush only, i.e. after the VM check, exactly where rechecks happen today. The
     pin rule of the argument above is untouched; the cache changes nothing about *which* TIDs are
     rechecked, only whether the page has to be read for them.
   - Why an answer keeps: `heap_hot_search_buffer(root TID, snapshot)` is a function of the snapshot.
     A tuple visible to us is not dead to all, so nothing may remove it; HOT pruning may rewrite the
     chain, but it only drops versions dead to *every* snapshot and leaves the survivors reachable
     from the root in order, so the version found from a given root does not change (the same
     guarantee a bitmap heap scan relies on between building its TID list and visiting the heap);
     a "not visible" answer cannot become visible, because every tuple written after we looked
     belongs to a transaction that is in progress at our snapshot, or began after it, or is ours
     with a command id at or above the snapshot's curcid; and line pointer numbers never move.
   - Serializable isolation: a cache hit calls neither HeapCheckForSerializableConflictOut() nor
     PredicateLockTID(), so filling an entry takes `PredicateLockPage(heap, blk, snapshot)` once.
     Both directions of rw-conflict detection are then closed: a write that happened BEFORE the page
     was resolved is caught by the conflict-out check inside the sweep (which walks every chain on
     the page, a superset of what any one recheck tests), and a write after it by the writer's own
     CheckForSerializableConflictIn() against that page lock. Page locks may be coarsened to a
     relation lock by SSI, which is conservative.
   - When it fills: the sweep costs one visibility test per line pointer instead of one per wanted
     TID, so it is only done once reuse is evident - the first count records nothing, the second
     records the blocks it visits, and a block is resolved on its second visit. A one-shot count
     therefore behaves exactly as it did before the cache existed, down to the last buffer visit.
   - Lifetime: one handle per count node execution, created by the caller
     (`lion_vis_cache_create`/`_destroy`) and passed to `lion_count_sources_cached()`; NULL means no
     cache. The handle empties itself when it is handed a different relation or a different
     snapshot, so the partitioned counts of §16 may share one handle and get one cache per
     partition. `lion_index_count_group_stats(idx, use_cache)` is the SQL image of that driver,
     for tests: same entry scan, one count per group, one cache.

SQL surface for tests: `lion_index_count(idx regclass, key anyelement) RETURNS bigint` and
`lion_index_count(idx1 regclass, key1 anyelement, idx2 regclass, key2 anycompatible) RETURNS bigint`,
whose keys are of two unrelated polymorphic types because each is compared with its own index's
column and the two columns need not share a type. Both open the heap via IndexGetRelation with
AccessShareLock, use GetActiveSnapshot(), and must return exactly `count(*)` of the equivalent
SELECT.
**The key's type is resolved as `col = key` would resolve it** (`lion_count_key_type()`, 2026-09-25
review; before it the key had to BE the index's opcintype or have a cross-type member, so enum_ops, a
DEFAULT class, could not be counted at all and neither could a varchar key on the varchar column it
indexes). A domain is its base type. For a class on a POLYMORPHIC type (enum_ops) the key must be of
the column's actual type and nothing else: a different enum would pass for "an enum", and its OIDs
mean nothing to this column (the DETAIL names the column's type, not anyenum). Otherwise the key is
of the class's own type, or of a type the family has a strategy-1 member for with it (int8 on int4:
`int48eq`, which §21's probe resolves further), or - lacking one - BINARY-coercible to the class's
type where the parser makes that coercion itself: its `=` for (column type, key type) must be the
class's own (opcintype, opcintype) operator reached without a cast function (`compatible_oper()`),
as for varchar on text_ops, which the parser relabels to call `texteq`. A cast function is never
taken (§21), and a coercion merely existing is not enough: text is binary-coercible to bpchar, but
`bpcharcol = 'x '::text` is `text = text` on the column cast by rtrim1(), and matches none of the
rows bpchar's own equality would count. The resolved type is what the lookup is made as and what
the EXECUTE check names the equality by (enum_eq, texteq; test/sql/security_exec.sql).
`lion_index_count_any()` resolves its array's element type the same way.
**A NULL argument answers NULL**, where `count(*) WHERE col = NULL` answers 0: the functions are
STRICT, deliberately (2026-09-25 review, kept). PostgreSQL never calls a STRICT function with a NULL
argument - a NULL constant folds the call away when the query is planned - so no count is made,
nothing is locked or checked, and NULL says exactly that. Answering 0 instead would mean deciding
what a keyless count checks, and the query it would stand for does not settle it: the planner folds
`col = NULL` to a constant-false filter, so that query reads no index, calls no equality and never
asks whether a materialized view is populated.
A NULL ELEMENT of `lion_index_count_any()`'s array selects nothing, as in `= ANY (...)`, and counts
0; a NULL array is a NULL argument.
They, `lion_index_count_any()` and `lion_index_count_group_stats()` refuse a MULTI-KEY column (§17):
its entries are extracted keys, not column values, so a whole tsvector as the search key matched no
entry's meaning and used to be hashed and compared as if it did. All of them refuse a materialized
view created WITH NO DATA with core's error ("has not been populated"), as ExecOpenScanRelation()
refuses the query, between the privilege checks and the EXECUTE checks where the executor raises it;
its heap and indexes are empty, so the count used to answer 0 (2026-09-25 review; the pushdown node
already refused it).
Nobody vetted the index they were handed, so they also make the decision the planner makes in
get_relation_info() before looking anything up: `lion_index_usable(index, snapshot, &why)` (lion.h,
implemented in lion_pages.c, shared with lion_index_verify's heapallindexed pass) requires
indisvalid and indisready and applies the indcheckxmin rule against TransactionXmin exactly as
plancat.c does; an unusable index is an ERROR naming the reason, never a count. This is not an
optimisation: an index built from a broken HOT chain holds only the latest version of that chain, so
an older snapshot's row is not in the posting set and no amount of heap rechecking can put it back
(the 2026-09-20 review reproduced `count(*) WHERE k = 1` = 1 against `lion_index_count()` = 0;
test/isolation/count_checkxmin.spec is that case).
Isolation tests must cover: concurrent uncommitted insert (not counted), committed insert (counted
in a new snapshot, not in an old REPEATABLE READ one), delete + VACUUM racing with the count (a
cursor or `pg_sleep`-free spec using isolationtester steps: s1 opens a REPEATABLE READ transaction
and counts once, s2 deletes rows and commits, s3 runs VACUUM, s1 counts again and must still see the
old value; then in READ COMMITTED s1 must see the new value), and an index the caller's snapshot may
not use at all (count_checkxmin.spec: a HOT update before CREATE INDEX makes the build set
indcheckxmin, and the old REPEATABLE READ reader must get the eligibility error rather than a count
that is missing the row it still sees).

## 10. Phase 2b: CustomScan for `count(*) [GROUP BY k] FROM t WHERE k1 = c1 AND ...` (`lion_customscan.c`)

Planner integration
- Install `create_upper_paths_hook` (chaining to any previous hook) at `_PG_init`; act only for
  stage UPPERREL_GROUP_AGG. GUC `pg_lion.enable_count_pushdown` (bool, default on).
- Applicability, all required (bail out silently otherwise):
  - input_rel is a single base relation (RELOPT_BASEREL, RTE_RELATION, relkind ordinary table or
    materialized view) — no joins, no subqueries, no old-style inheritance parents. §16 added
    partitioned parents, which are counted one live leaf partition at a time; everything this
    section says about "the relation" then means "the partition being counted". §27 added one
    join shape - a fact table joined to a dimension on a lion-indexed fk - where "the relation"
    is the fact table and the dimension side is a child plan.
  - The relation's table AM is the heap, by routine (§2). ambuild already refuses any other, so
    this only declines an index that got past it; the executor checks again when it opens the
    relation.
  - The query has no DISTINCT, window functions, grouping sets, ORDER BY inside aggregates,
    FILTER clauses, or aggregates other than `count(*)` and the `count(col)` cases of §14.
  - **HAVING** is accepted when it is a filter the node can apply itself. By the time the hook runs,
    `havingQual` is an implicit-AND list of only those clauses that mention an aggregate (or are
    volatile, or contain a subquery): `subquery_planner()` has moved every other clause into WHERE,
    so `HAVING a > 3` reaches us as a WHERE inequality and is refused like any other. Of what is
    left, every Var must satisfy the target-list rule below (a group column, or a column a clause
    pins to one value, whose printed value is the stored key) and every aggregate must pass the
    `count` test of §14; a SubPlan (a correlated subquery, evaluated per group) is refused, while an
    uncorrelated one is an InitPlan by then and arrives as a Param, which is a plain value. The
    accepted list becomes the CustomScan's `plan.qual`; setrefs.c rewrites its Aggrefs and Vars into
    INDEX_VAR references against `custom_scan_tlist`, which is why the plan step adds every column
    and count the HAVING mentions to that list even when the target list does not print it (an
    Aggref left unmatched there would reach `ExecInitExpr`, which only an Agg node may do).
    `lion_emit_tuple()` evaluates the qual on the finished group's scan tuple; a group that fails
    is consumed like any other and the outer fetch loop asks for the next one, counting it in
    `Rows Removed by Filter`. The cost adds the qual's evaluation per group and scales the row
    estimate by `clauselist_selectivity()`, as `cost_agg()` does for an Agg's quals. A partitioned
    table's node emits partial counts, so its HAVING goes to the Finalize Agg above it instead
    (§16), and the partial target carries the HAVING's counts as well - core's
    `make_partial_grouping_target()` does the same with the havingQual.
  - Every baserestrictinfo clause is `Var opeq Const` or `Const opeq Var` where Var is a plain column
    of the rel with a *valid* lion index whose opfamily contains that operator as strategy 1 (use
    the index's opfamily and the operator OID; cross-type integer equality is fine because the
    integer opfamily contains it), the compared value is not a literal NULL, and there is at most
    one clause per column (two different values on one column ⇒ bail; the same value twice ⇒
    dedupe, by `equal()`, so two different Params on one column bail). §15 adds
    `Var = ANY (array)` and §14 the two null tests to the shapes accepted here; an `IS NOT NULL`
    clause is exempt from the one-clause-per-column rule, because it constrains no value. §19 adds a
    top-level `OR` of such clauses, whose leaves are exempt from the rule as well and enter none of
    the per-column bookkeeping, because they constrain no column of the result.
  - **An index is an (index, KEY COLUMN) pair since §24.** A multicolumn lion index holds each of
    its columns' keys as an independent set of entries, so `lion_find_roaring_index()` accepts a
    match on ANY key column and returns its number `i` beside the index; every opclass question in
    this section is then asked of THAT column - `opfamily[i]`, `opcintype[i]`,
    `indexcollations[i]` - and every lookup, entry scan and `datumCopy()` of that clause names the
    column (`lion_posting_set_lookup_col()`, `lion_entry_scan_begin_col()`,
    `lion_index_column_state()`). Two clauses of one query may name the same index; a query
    constraining three columns of one index is three posting-set sources located through one
    relcache entry, ANDed exactly as three separate indexes would be. The column is NOT carried in
    `custom_private`: the executor derives it in `lion_open_relation()` from the index it really
    opened and the clause's heap attnum, because a partition's index may put the same heap column
    at a different position from the parent's (§16) - and may number the heap column differently
    too, which is resolved by name.
  - **A Param stands wherever a Const may** (2026-09-21 follow-up review). A prepared statement's
    GENERIC plan keeps `k = $1` as a Param - that is what a generic plan IS - and accepting only
    literals meant the node was never used by one: a dense count fell back to the ordinary plan and
    measured **5.6 ms against 0.025 ms** on 100k rows, and 207 ms against 3.1 ms on five million.
    Both parameter kinds are accepted, PARAM_EXTERN for a statement's own parameters and PARAM_EXEC
    for the ones a nested loop or a LATERAL reference supplies, for equality and for the array of an
    IN list; `k IN ($1, $2)` keeps an `ArrayExpr` over them in a generic plan, and that is accepted
    too, as long as its elements are themselves literals or parameters (the node evaluates the array
    ONCE per scan, and an element that could be volatile does not mean the same thing evaluated once
    as it does evaluated per row). A multi-key clause (§17) still requires a Const, because there
    the query's SHAPE and not just its value decides whether the posting sets can answer it at all.
    At plan time the value is unknown, so `clause_selectivity()` gives the estimate it gives any
    non-Const comparison; at run time a NULL value selects no rows, which every operator involved
    agrees with by being strict.
  - Collations follow the planner's IndexCollMatchesExprColl() rule: a collation-sensitive clause
    (OpExpr/ScalarArrayOpExpr inputcollid valid) or grouping column may only use an index whose
    indexcollations[i] - of the KEY COLUMN that indexes the clause's column (§24) - equals that
    collation, because the index hashed and compared keys under its own collation and the count
    never rechecks the predicate. Checked per partition.
  - GROUP BY is empty, or exactly one plain Var of the rel with a lion index - or two of them,
    which §20 answers as a nested loop over the two indexes' entries, applying every rule of this
    section per column. Since §24 the two may be two KEY COLUMNS of ONE multicolumn index, which is
    that feature's natural use: the relation is opened twice, each side walks its own column's
    entries, and the outer/inner choice is made from the columns' cardinalities exactly as it is
    between two indexes. A grouped column
    may also appear in the WHERE clause (then it is a single group). §14 removed the `attnotnull`
    requirement: the NULL group comes out of the reserved NULL entry.
  - **The driving index's equality is the grouping equality** (2026-09-20 review, finding 3). An
    index's entries are the classes of
    strategy 1 of ITS opfamily on its own key type, and an operator class is free to define a
    coarser equality than the type's (a text opclass over `lower()` is a valid opclass, and the
    2026-09-20 review built one: 50k `'A'` and 50k `'a'` came out as one group of 100k). So the
    equality operator the planner chose for the grouping column - `SortGroupClause.eqop`, which is
    the type's own - must be exactly the operator
    `get_opfamily_member(idx->opfamily[i], opcintype[i], opcintype[i], 1)` returns for the driving
    KEY COLUMN i (§24), compared by Oid;
    cross-type equality does not arise for a group column. Matching collation is NOT enough. The
    check is per relation, so every partition's own index is checked (§16), and it covers the
    `count(col)` cases of §14 that read the group column's entries, since those are only reached
    through a grouping index. A sum-over-all driver (§14) is exempt: the entries of one index are
    disjoint and cover every row however they partition it, so their total does not depend on the
    equality. Without GROUP BY nothing else does either - counting a class needs no assumption
    about how the class was formed, only that the clause's operator is the index's strategy 1.
  - The planner's grouped_rel target contains only the GROUP BY column(s) and the aggregate(s).
- **Value-producing pushdown has a stricter contract than counting** (2026-09-20 review, finding 4).
  A posting set stores ONE representative key per equality class, chosen when the entry was created,
  and nothing keeps a visible row with that exact representation in it: indexing citext
  `'SecretOldSpelling'`, deleting it, inserting 10,000 `'secretoldspelling'` and vacuuming leaves an
  entry whose key is a spelling the table no longer contains. So a pushdown that PRODUCES a value -
  the GROUP BY column in the output target, or a column a `=` clause pins whose value the target
  list prints from the entry's stored key - is only built when equality on that index implies an
  identical binary representation. Both halves are checked, per relation, in `lion_collect_targets()`:
  - the index's own equality (strategy 1 for (opcintype, opcintype)) IS the type's equality, i.e.
    the equality of the type's default btree opclass. Otherwise "same entry" is weaker than "equal"
    and the question below is not even the right one;
  - and that equality is representation-preserving, decided exactly as btree deduplication decides
    it in `_bt_allequalimage()`: the type's default btree opfamily (`lookup_type_cache` with
    `TYPECACHE_BTREE_OPFAMILY`), its `BTEQUALIMAGE_PROC` (support function 4) for (type, type),
    called under the INDEX's collation - which is how `btequalimage`/`btvarstrequalimage` decide
    determinism. A missing support function or a false answer (citext, numeric `1.0`/`1.00`,
    nondeterministic collations) means no.
  - and, before either is asked, the type is on an explicit list of core types whose equality
    compares every byte the output function prints (2026-09-23 security review):
    `lion_type_equalimage()` names them and argues each. equalimage only promises that equal
    values are interchangeable for deduplication, which is weaker: bpchar registers
    `btvarstrequalimage`, yet `'a   '` and `'a'` are equal and print differently, so a GROUP BY
    printed a deleted row's padding. bpchar and every extension type are refused.

  A count-only pushdown - no group column in the output and no pinned column echoed - is unaffected:
  every member of the class is counted whatever it is spelled like. `IS NULL` is exempt as well:
  NULL has one representation. Regression coverage is in `test/sql/citext.sql` (the review's
  representative case) and `test/sql/pushdown.sql` (numeric, and a text GROUP BY that still pushes
  down).
- Add a CustomPath with `flags = 0` (no parallel, no backward, no mark/restore), `pathtarget =
  grouped_rel->reltarget` (a partitioned GROUP BY uses a partially-grouped target instead and is
  wrapped in a Finalize Agg: §16), rows = estimated groups (from estimate_num_groups, or 1 without
  GROUP BY),
  and a cost (`lion_cost_count_rel()`, per relation, summed over the leaves of §16) that is the sum
  of what the node really reads:
  - **one bucket page per looked-up key** at random_page_cost, and that key's own container chain -
    written sequentially, so its share of the index's container pages, at least one page - at
    seq_page_cost. An IN list is one such lookup per element (§15) and the bucket pages are shared
    once the list is longer than the index has buckets, so the pages are `Min(nelems, nbuckets)`.
    Since §22 only the source that DRIVES the merge pays for its whole chain: the others are SOUGHT
    to the driver's container keys and pay for the leaves those probes touch, which is the formula
    in §22's cost model and never more than the chain itself;
  - **every page of the group index** for a GROUP BY, at seq_page_cost: its entries are all walked;
  - **and all of those page terms are scaled to ONE KEY COLUMN of a multicolumn index** (§24).
    `idx->pages` and `lion_index_dir_pages()` are per RELATION, and a multicolumn index is one
    relation holding n independent sets of entries, so charging a column the whole directory and
    the whole page count prices `WHERE b = 1` on `(a, b, c)` at three times what the same query on
    a single-column index of `b` costs - and the node is then refused for a query it answers
    exactly as fast. The share is that column's estimated fraction of the relation's entries,
    `n_distinct(col) / sum of n_distinct over the index's key columns`, taken through
    `examine_variable()`/`get_variable_numdistinct()` on a Var of this relation's own attribute
    numbers (the partition's, for a partition). It scales the clause's directory and container page
    terms and the entry scan of a driving column, and nothing else: the directory HEIGHT is not
    scaled, because a descent passes through the upper levels the columns share, and neither is the
    index's own size where `lion_heap_page_cost()` uses it to decide how much of it is cached,
    which is a property of the relation. **The limitation** is that n_distinct is not an entry
    count: a multi-key column (§17) has one entry per LEXEME rather than per row value, so its
    share is understated and the scalar columns beside it are charged for its directory; a column
    with no statistics falls back to DEFAULT_NUM_DISTINCT for itself alone, which makes the split
    equal when NO column has statistics and biased when only some do. Both errors are bounded by
    the number of columns, which is exactly the error the correction removes - without it, the
    factor is the column count, always, and always against the node. `test/sql/multicolumn.sql`
    pins it by asking the same question of one multicolumn index and of n single-column ones and
    requiring the same plan choice; the five-clause case there is refused over the multicolumn
    index with the correction disabled and accepted with it (2026-09-22);
  - **one O(1) step per container per participating source**, twice cpu_operator_cost (a block mask
    and a visibility-map mask), where a source's containers are `Min(heap_pages /
    LION_BLOCKS_PER_CONTAINER, its members)`, so a GROUP BY pays numgroups of them;
  - **the union of an IN list**, cpu_operator_cost × members × log2(nelems): a merge of k sets costs
    that per member however it is organised (§15 builds the k-way one), and a single-key clause with
    k = 1 pays nothing for a merge it does not make;
  - **the heap the visibility map cannot vouch for**: `recheck_tids = rows × dirtyfrac` from
    `pg_class.relallvisible/relpages`, one cpu_tuple_cost each - that is a visibility-bit lookup in
    the per-query cache of §9 - on `Min(recheck_tids, heap_pages × dirtyfrac)` DISTINCT pages,
    fetched ONCE per query whatever brings the count back to them.

  That last bound is the whole of the 2026-09-21 follow-up review's grouped-count finding. The node
  used to be charged `Min(numgroups × heap_pages × dirtyfrac, recheck_tids)` page visits at
  random_page_cost, on the grounds that each group's merge returns to a dirty block separately. It
  does, but §9's visibility cache resolves a dirty page once per snapshot and answers every later
  group out of memory, so those are repeated CPU and not repeated I/O. At 5M rows, 200 groups and 5%
  of the rows updated (91,345 of 100,407 pages all-visible) the old model asked **1,815,739** cost
  units against the sequential aggregate's 175,511 - for a node that ran in **148 ms against
  1,174 ms**, with 418,819 buffer hits and zero physical reads over a 71 MiB working set. The same
  shape reproduced here: **15,661 against 155,840**, and 141 ms against 850 ms.

  The page cost respects caching for the same reason (`lion_heap_page_cost()`): it interpolates
  between seq_page_cost and random_page_cost on whichever of two ratios argues more strongly for
  sequential access - the working set's share of this relation's prorated `effective_cache_size`, as
  `index_pages_fetched()` prorates it, and the fraction of the relation the set covers, as
  `cost_bitmap_heap_scan()` interpolates it. A dirty working set much larger than the cache is still
  charged as random I/O, which is the failure a blanket preference for this node would hide.

  The node streams: every row is emitted as it is counted, so the startup cost is 0 except for the
  forms that produce a single row (a plain count, or a GROUP BY the planner folded to one group),
  which cannot emit anything before the whole scan is done. There is no per-group memory to charge
  beyond one group's iteration state, and nothing above the node is priced here - a partitioned
  GROUP BY puts core's Finalize Agg on top and core costs that (§16).

  This is deliberately optimistic but proportional; document it. `test/sql/pushdown.sql` pins every
  end of it with nothing disabled, so that which plan the cost model picks IS the test:
  - a 200k-row table vacuumed and then slightly extended (relallvisible just under relpages) must
    choose the node for a single count and for a small GROUP BY;
  - a 100k-row table vacuumed and then 5% of its rows updated - about a tenth of its pages dirty -
    must choose it for a 20-group GROUP BY (2.0 ms against 15.8 ms);
  - a 1000-group never-vacuumed table must choose it as well, which is the answer that CHANGED with
    the visibility cache: one pass over the dirty pages plus 100,000 bit lookups is 17.9 ms against
    the sequential aggregate's 26.8 ms;
  - and a 20000-group never-vacuumed table must still lose to it, because there the per-group work
    and not the recheck is what costs: 40.8 ms against 28.0 ms. Higher-cardinality groupings lose by
    more (at 5M rows, GROUP BY over 20000 keys is 1,541 ms against 1,114 ms and over a million keys
    4,654 ms against 2,047 ms), and the model refuses all of them.
- PlanCustomPath produces a CustomScan with `scan.scanrelid = 0` (upper-level node),
  `custom_scan_tlist` = the output columns (group key Var(s) with their original varno/varattno, and
  the aggregate as a Const-shaped placeholder replaced at execution), and `custom_private` holding:
  the heap relid, the list of (index oid, attnum) for WHERE, the group-by (index oid, attnum) pairs
  - two of them since §20 - or none, and the aggregate kinds.  The clause VALUES travel in
  `custom_exprs` rather than in
  `custom_private`, because that is the only field of a CustomScan the planner's later passes look
  inside: `set_customscan_references()` fixes its expressions up and `SS_finalize_plan()` collects
  the Param ids it finds there into the plan's extParam/allParam, which is what makes the executor
  rescan this node when an exec Param changes.  A value left only in `custom_private` would be
  invisible to both, and a parameterised inner side would then count the first outer row's key
  again. Use `build_path_tlist`-style
  handling with INDEX_VAR references in the plan's targetlist as pg_strom/TimescaleDB do.
  `custom_private` is a POSITIONAL list (the `LION_PRIV_*` indexes), so its first member is a shape
  marker - an IntList of `LION_PRIV_MAGIC` and the number of members - which BeginCustomScan checks
  before reading any offset and ERRORs on. §16 added `LION_PRIV_PARTS` and §19 `LION_PRIV_ORS`, the
  structure of each OR restriction over the flattened clause array. Planner/executor drift, or a plan built by a differently
  shaped build of the library, is then a message and not a misread Oid.

Executor
- BeginCustomScan: open heap with NoLock (the executor already locked every RTE); index_open each
  index with AccessShareLock; register the executor snapshot (estate->es_snapshot); allocate work
  buffers, a per-group memory context and one visibility cache for the whole execution (§9); and
  take each clause's value from `custom_exprs` - a Const's is read straight out of it, anything else
  gets an `ExecInitExpr()` ExprState.
- The parameters are evaluated at the START of each scan, not in BeginCustomScan: an exec Param is
  set by the nested loop after the node has been initialised and again before every rescan, so the
  values are computed on the first ExecCustomScan call after each ReScan
  (`ExecEvalExprSwitchContext()` through the node's own ExprContext, then `datumCopy()` into a
  context of the node's that lives exactly as long as the scan - the per-tuple memory the evaluation
  leaves its result in belongs to nobody here). ReScan throws them away with everything else.
- ExecCustomScan without GROUP BY: on the first call compute lion_count_keys for the WHERE keys and
  return one tuple (count); subsequent calls return NULL.
- ExecCustomScan with GROUP BY: iterate all entries of the group-by index in bucket order (walk
  bucket pages; copy each entry's key and head/inline payload while pinned as in §9). For each
  entry, count the AND of its posting set with the WHERE posting sets (§9). Emit (key, count) only
  when count > 0 (a group exists only if at least one row is visible). Output order is arbitrary;
  the planner must not assume sortedness (pathkeys = NIL). This is one function,
  `lion_next_group()`, and it is the whole of the GROUP BY executor: a partitioned scan runs it once
  per partition (§16).
- ReScanCustomScan: reset iteration state. EndCustomScan: close indexes, free.
- ExplainCustomScan: print "Indexes: idx1 (col = const), ..." - with the index's KEY COLUMN
  appended as `idx1.col` when, and only when, the index is a MULTICOLUMN one (§24), so that two
  clauses answered by one index can be told apart and every plan written before §24 is unchanged -
  and "Group Key: col" and, with ANALYZE,
  the number of TIDs rechecked in the heap, the heap blocks skipped via the visibility map, the
  containers visited, and the block visits the visibility cache answered or had to let past its
  budget ("Heap Blocks From Cache" / "Heap Blocks Past Cache Budget", §9). A clause whose value is
  not a literal is printed as the expression the plan carries, which for a prepared statement's
  parameter is `$1` - the text core's EXPLAIN gives a qual on one - via `deparse_expression()`
  against the plan's own deparse context.

Tests (pg_regress): the pushdown produces identical results to the plain plan for: no rows; all rows;
WHERE constants that match no key; cross-type constants; GROUP BY with and without WHERE; after
DELETE without VACUUM (recheck path) and after VACUUM (VM path); EXPLAIN shows the custom node when
`pg_lion.enable_count_pushdown = on` and the normal plan when off.  Parameters get the same
treatment under `plan_cache_mode = force_generic_plan`, which is what keeps a `$n` a Param: a
count, a count that matches nothing, a NULL parameter, `= ANY ($1)`, `IN ($1, $2)`, a GROUP BY with
a parameterised WHERE clause, a parameterised clause whose column the target list prints, and a
LATERAL nested loop whose inner side is rescanned with a new exec Param for every outer row - each
compared against the same query with the pushdown switched off, as a multiset both ways round. `test/sql/null.sql` and
`test/sql/inlist.sql` do the same for the clause kinds of §14 and §15, comparing every query against
a forced sequential scan rather than against the pushdown-off plan, so that the access method's own
answers are checked too. Section 11 of `test/sql/multicolumn.sql` runs the whole of this section
over ONE multicolumn index (§24): a count and a GROUP BY per column, several columns ANDed, a
two-column GROUP BY out of one index, IN lists on the group column and on another column of the
same index, the null tests including two `IS NOT NULL`s over one index, an OR across two of its
columns, parameters, a partitioned table whose partitions order their index columns differently
and number their heap columns differently again, a dirty heap and a clean one - each against the
same query with the pushdown off, as a multiset both ways round - and it pins the plan CHOICE
against a twin table carrying one single-column index per column.

## 11. Additional VACUUM rule for phase 2 (binding on wave 2 `lion_vacuum.c`)

ambulkdelete acquires a cleanup lock (an exclusive content lock that also waits for all other pins
to drop) on **every page that can hold a TID** — every directory LEAF and every LEAF of every key's
posting tree, whether or not it has anything to remove there — in chain order (the directory leaves
left to right from the leftmost one, then each posting set's leaves left to right from ITS leftmost
one).

**Chain order is leaf right-link order** (§22). A posting set is a B-tree over container keys now,
but its leaves are the container pages that were the chain, they are still rightlinked in ckey
order, and VACUUM still walks exactly that list — it only has to DESCEND to the leftmost leaf
instead of starting at the entry's `head`, which is the tree's root.

**Internal DIRECTORY pages are not cleanup-locked, and need not be** (§21): they hold downlinks and
separator keys and no TIDs at all, and a directory page never changes level - a root split keeps
the old root as the left half at its own level - so no reader can be holding a container copy that
came from one. The directory leaves are exactly the pages that used to be bucket pages, and VACUUM
treats them the same way. **A posting tree's descent IS cleanup-locked, root first** (§22, and a
deviation from this section's first version, which said internal posting pages "need not be"
either): a posting set's root is a LEAF for as long as the set fits one page, a reader that copied
containers out of it pins it, and an insert that overflows it pushes it down - moving those very
containers to a brand new child - under an exclusive lock the pin does not stop. A VACUUM that
walked from the leftmost leaf would never ask for the old root at all, clean the child, and let the
heap phase mark the rows all-visible under the reader
(`test/isolation/count_root_pushdown_race.spec` answered 1000 for 500). So pass 2 descends from the
root to the leftmost leaf with a cleanup lock on every page of the way (`lion_vacuum_descend()`),
and the reader's pin on the old root stops it before it reaches the page the containers moved to.
The root is the only page that ever changes level; the internal pages below it are locked for
uniformity, and there is one of them per several hundred leaves. A leaf SPLIT obeys the same rule the chain split does - the upper half goes to a
brand new page immediately to the right - so an entry can only ever move onto a leaf the walk has not
passed, and an entry that moves off a leaf the walk has already finished has by then been fully
processed, posting sets and all. This is the same rule nbtree's btvacuumscan follows, and it is what
closes the page-split hole in the §9 argument: a reader pins page P, copies container C out, and
drops the content lock; a concurrent insert may then split P and move C to a brand-new page N linked
immediately right of P. VACUUM must reach P before N, and P's cleanup lock is blocked by the
reader's pin, so it cannot clean C on N until the reader has finished its visibility-map checks.
If VACUUM had already passed P when the reader copied C, C had already been cleaned of this
cycle's dead TIDs. Two properties make this sufficient: a page is only ever linked into the leaf
list immediately right of the page whose split or spill allocated it, so chain order and VACUUM's
visit order agree, and each VACUUM cycle's set of dead TIDs is fixed before index cleanup starts. If
VACUUM only cleanup-locked pages it modified, it could walk past an emptied P and clean C on N while
the reader still counts from its stale copy.

**A reader that SEEKS rather than walks does not weaken any of that** (§22). The argument was never
about which pages the reader has visited; it is about the page the container it is holding came
from. A cursor that descends straight to leaf Z and takes container C from it pins Z, and VACUUM
cannot have cleaned Z - it has to hold Z's cleanup lock to remove a TID from it, and that lock waits
for the pin. If a split then moves C to a page right of Z, VACUUM reaches Z first, and is blocked
there. The internal pages the descent reads on the way hold no TIDs.

**A ROOT PUSH-DOWN is the one page transition VACUUM can meet** (§22): an insert turns a one-page
posting set's root into an internal page and moves its containers to a brand new child. VACUUM,
holding the cleanup lock on that page, finds it is no longer a leaf and descends again from the
root, cleanup-locking the way down. That is safe for the reason above: the cleanup lock it holds
waited for every pin on the page those containers came from, so no reader holds a stale copy of
them, and the page they moved to did not exist a moment ago. The push-down VACUUM itself causes -
its REGROW step (§18) re-placing a filtered container that no longer fits a one-page set - keeps
the new child EXCLUSIVE from its allocation until the filtered container is on it
(`lion_posting_root_pushdown()` returns it locked): the push-down has just copied the UNFILTERED
container there, and a reader that could lock the child in between would copy the dead TIDs, keep
its pin, and have the write that follows - which takes no cleanup lock - remove them under it.
(Found while auditing every record VACUUM writes through shared placement code;
`test/isolation/vacuum_regrow_pushdown.spec` parks the regrow between the push-down and the write
and sends a count's descent at the child: with the child unlocked there it answered 5,307 for
4,341, with it held the descent waits for the child and answers 4,341.)

Page recycling (§18) does not weaken either property. A recycled block is still linked immediately
right of its split origin, so it is still visited after it. A page only ever LEAVES the leaf list as
part of a whole-set free, which happens under a cleanup lock on that page after VACUUM has found it
empty - so a reader pinning it blocks the free outright, and a reader that got past it is holding
containers that VACUUM has already cleaned. And a page that a split moves items onto has had those
items cleaned already, because they come from a page this cycle visited first.

The guarantee readers may rely on is therefore: **a TID is never removed from any page of an index
before VACUUM has held a cleanup lock on every page that precedes it in chain order - the pages of
each posting tree's descent included - and the removal itself happens under a cleanup lock on that
page.** A reader that checks the visibility map before dropping the pin on the page a container
came from cannot be overtaken by VACUUM (§9).

**On a standby the same sentence holds for REPLAY of an rmgr-mode index** (§25). The startup
process is the only writer there, it applies the primary's records in the order the primary wrote
them - so it meets the pages in chain order for the same reason VACUUM did, and a page is still
only ever linked in immediately right of the page whose split created it - and it takes a CLEANUP
lock on every block a record removes TIDs or items from (VACUUM_PAGE, ITEM_DELETE, PAGE_DELETED,
the ENTRY record VACUUM deletes entries with, and every record VACUUM writes through shared
placement code: the regrow of §18 and an INLINE spill its filtering causes). **And on every page
VACUUM cleanup-locked without writing to it**: those write no record of their own, so VACUUM
collects them and the next record whose replay takes a cleanup lock carries them as a barrier -
ranges of blocks redo cleanup-locks one at a time before it touches the record's own blocks (§25,
`xl_lion_visit`). Without it the "every page that precedes it" half of the sentence above has no
standby counterpart, and a split, a root push-down or a spill that moved a standby reader's TIDs
off its pinned page lets replay remove them elsewhere. Pages visited after the last removal of an
ambulkdelete need no barrier - every dead TID of the cycle is gone by then - and are dropped.
§11's WAITING RULE applies to the startup process as well and is met the same way: the barrier
comes first, then the blocks that need a cleanup lock (the shim puts them FIRST in the record
whatever order the writer registered them in), so every wait happens with no other buffer lock
held. Replay of a GENERIC record takes an exclusive lock instead and offers none of this, which is
why §9's last rule is per resource manager.

VACUUM waiting rule (binding): VACUUM must never *wait* for a cleanup lock while holding any other
LWLock, because holding an LWLock implies HOLD_INTERRUPTS and the wait becomes uncancellable (a
cursor pinning one container page could then freeze VACUUM until the session ends). The leaf lock is
needed only while a page is actually modified (the entry counters are written in the same record),
so ambulkdelete processes each leaf in two passes:

  Pass 1 (directory leaves): pin the leaf; LockBufferForCleanup(leaf) with nothing else held;
  process every INLINE entry on it (filter, repack, spill if grown); note the offset, the head block
  and the KEY of every CHAIN entry; release the content lock but keep the leaf pinned; move to the
  right link.

  Pass 2 (each CHAIN entry's chain, left to right, holding no page lock between steps): for EVERY
  page X of the chain, including pages with nothing to remove:
    1. LockBuffer(leaf, EXCLUSIVE)  — blocking is fine, nothing else is held.
    2. if ConditionalLockBufferForCleanup(X): filter X; if anything changed, write the fresh entry
       copy (re-read from the page; inserts may have changed it) with the deltas in the same record;
       release X, release the leaf.
    3. else: release the leaf; LockBufferForCleanup(X) (blocking, cancellable, nothing else
       held); if ConditionalLockBuffer(leaf, EXCLUSIVE): as in step 2; else release X and go to 1.
  A page with nothing to remove still needs the cleanup lock (step 2/3) but no WAL record; the leaf
  lock may be skipped for such pages only if the page is inspected under the cleanup lock first and
  found clean (then release and move on).
  Inserts may interleave between steps; that is safe because splits only move items right to new
  pages (VACUUM revisits or re-filters them). Offsets of entries are NOT stable any more - §21's
  sorted directory inserts in the middle of a leaf and splits it - so every use of the offset pass 1
  recorded is checked against the entry's kind and stored key first, and an entry that has moved is
  found again by a descent made with NOTHING else held (a directory lock taken while holding a
  container page would close exactly the cycle the waiting rule above avoids). Counters are always
  applied as deltas to the entry as it is at the moment of the write, never as absolutes.

This also removes the latent deadlock between a reader that pins a directory leaf (INLINE entry)
while taking a container-page SHARE lock and a VACUUM holding that container page while waiting for
the leaf: VACUUM never blocks on the leaf while holding a container page.

**Parallel VACUUM does not change any of this** (§18, "Measured, 2026-09-23"). Since then lion
declares `VACUUM_OPTION_PARALLEL_BULKDEL`, so a VACUUM of a table with several indexes hands each
index's ambulkdelete to whichever participant - the leader or a parallel worker - claims it first.
Everything above is a statement about ONE index, and one index is still vacuumed start to finish by
one process: the same chain order, the same cleanup lock on every page, the same waiting rule, the
same per-process standby barrier (`lion_wal_visit()` keeps one list per backend and one index at a
time). Two things could have made the process matter, and neither does. The dead-TID set is still
fixed before any index is cleaned (the workers read the leader's store and nobody adds to it until
every index is done), and the heap is still only marked all-visible after `lazy_vacuum_all_indexes()`
has waited for every participant, so a reader's pin still holds the whole cycle off from making its
TIDs' heap pages all-visible - whichever process it is blocking. A reader that holds pins in two
indexes (an AND across two columns) can now block two participants at once instead of one after the
other, which is the same wait. `amvacuumcleanup` stays with the leader: it only vacuums the free
space map. `test/isolation/vacuum_parallel.spec` makes a worker run a lion ambulkdelete and checks
what it leaves.

Deadlock rule for readers: never acquire a directory lock while holding a pin on a container page.
VACUUM holds the leaf and then waits for cleanup locks on that entry's container pages; a reader
holding a container pin and then asking for the leaf closes the cycle, and buffer LWLocks have no
deadlock detection. Finish with the leaf (copy the entry out, drop its lock) before pinning
container pages, and never go back. `lion_chain_find_page()` takes SHARE locks
internally, so do not call it while holding a lock on any page of that chain.

### On-access pruning sets the visibility map too (PostgreSQL 19+)

§9 was written when VACUUM was the only thing that set a VM bit. PostgreSQL 19's
`heap_page_prune_opt(rel, buf, &vmbuf, rel_read_only)` (pruneheap.c) passes
`HEAP_PAGE_PRUNE_SET_VM` when the scan's relation is read-only for the query, and
`heap_page_prune_and_freeze()` then marks the page all-visible if what is left after pruning is.
Seq, bitmap-heap, index and index-only scans all call it (heapam.c `heap_prepare_pagescan()`,
heapam_handler.c `BitmapHeapScanNextBlock()`, heapam_indexscan.c), so on 19 pages our
counts read are made all-visible by core scans whatever pg_lion does, and since this section the
count's own heap recheck does it too. The claim §9 needs, and that this section proves, is:

> **At any moment at which a heap page is all-visible, every TID that a lion index holds for that
> page and that the counting reader holds a pin for (§9) is exactly one row visible to every
> snapshot, the reader's included.**

On 16-18 on-access pruning never sets the VM (the function has no `vmbuffer` argument there), so
this section is about 19 and later; checked against 19's and 20devel's pruneheap.c, which differ
only in one `visibilitymap_clear()` call.

**What the index holds.** A lion TID is always a ROOT line pointer: the index is not summarizing,
so a HOT update inserts no index entry and a heap-only tuple is never pointed at. (A heap-only
tuple's slot may later be freed and reused, but reuse means a new tuple inserted with its own
index entries, not an existing entry now naming it; see "LP_UNUSED" below.)

**What an all-visible page may contain after on-access pruning.** `heap_page_prune_and_freeze()`
plans every line pointer (`prune_freeze_plan()`) and sets the page all-visible only if
`set_all_visible` survives all of the following, each of which clears it:
- an LP_NORMAL tuple left on the page that is not `HEAPTUPLE_LIVE`
  (`heap_prune_record_unchanged_lp_normal()`: RECENTLY_DEAD, INSERT_IN_PROGRESS and
  DELETE_IN_PROGRESS all clear it), or is LIVE but not yet hinted xmin-committed;
- the newest xmin of the live tuples still being considered running by any snapshot
  (`GlobalVisTestXidConsideredRunning(vistest, newest_live_xid)`, after the plan) - the
  reader's registered snapshot advertises its xmin in its PGPROC, so a row committed after the
  reader's snapshot keeps the page off the map;
- **any LP_DEAD item at all**, whether pruning made it just now or it was there already:
  `heap_prune_record_dead()` and `heap_prune_record_unchanged_lp_dead()` both append to
  `deadoffsets`/`lpdead_items`, and `if (prstate.lpdead_items > 0) set_all_visible = false`
  runs before `heap_page_will_set_vm()`, with `Assert(!set_all_visible || lpdead_items == 0)` after
  it and a `heap_page_is_all_visible()` cross-check on assert builds.
DEAD tuples themselves never survive into the all-visible page: a DEAD root (an aborted insert, a
deleted or non-HOT-updated row whose deleter is older than every snapshot) becomes LP_DEAD, which
by the last bullet blocks the bit; a chain whose root is followed by dead versions is redirected
to its first live member, and the dead HEAP-ONLY members become LP_UNUSED.
So on an all-visible page every root line pointer is one of: LP_UNUSED; LP_NORMAL holding a tuple
visible to all; LP_REDIRECT to a chain whose surviving members are all visible to all.

**Each of those, as a TID in the index.**
- *LP_NORMAL, visible to all*: one row, visible to the reader. It cannot also be HOT-updated to
  another visible version: a committed updater makes it at least RECENTLY_DEAD, and an aborted
  updater leaves a DEAD heap-only successor that `prune_freeze_plan()` frees separately.
- *LP_REDIRECT*: `heap_prune_chain()` redirects the root to the first non-DEAD member and frees
  the dead ones before it; for the page to be all-visible that member is LIVE, and it is the only
  live one for the reason in the previous point. One row. (A redirect whose target is gone becomes
  LP_DEAD, `heap_prune_chain()`'s `nchain < 2` case, and blocks the bit.)
- *LP_UNUSED*: the one case that would count a row that does not exist, so it must not happen
  while the reader's pin is held. On-access pruning never makes a root LP_UNUSED: it frees only
  heap-only tuples - the members after the root in `heap_prune_chain()` and the unchained DEAD
  heap-only tuples in `prune_freeze_plan()`'s last loop, and `heap_page_prune_execute()` asserts
  exactly that - while a dead ROOT goes through `heap_prune_record_dead_or_unused()`, which picks
  LP_DEAD unless `HEAP_PAGE_PRUNE_MARK_UNUSED_NOW` was passed. `heap_page_prune_opt()` never
  passes it ("cannot safely determine that during on-access pruning"), and VACUUM passes it only
  when the relation has no indexes (`vacrel->nindexes == 0`), which a relation with a lion index is
  not. LP_DEAD to LP_UNUSED is VACUUM's second heap pass (`lazy_vacuum_heap_page()`, the
  `lp_truncate_only` path), which runs only after every index's ambulkdelete for that cycle -
  and ambulkdelete cannot pass the page our container came from while we pin it (§11 above). So a
  pinned container's TID is never LP_UNUSED, and nothing about that changed in 19.
- *LP_DEAD*: excluded by the page being all-visible.
- *A heap-only tuple*: excluded by "what the index holds".

**Every other path to the bit.** Besides on-access pruning, 19 sets the VM in VACUUM's first pass
(the same `heap_page_prune_and_freeze()`, with the same LP_DEAD rule), in its second pass (after
ambulkdelete), for empty pages, and in `heap_multi_insert()` for COPY FREEZE into a table created
in the same transaction - none of which depend on who pruned. Nothing else calls
`visibilitymap_set()`. `heap_page_fix_vm_corruption()` and the fast path for already all-visible
pages only ever clear. An insert, update or delete still clears the bit under the heap page's
exclusive lock, so the "stale true" case of §9 is unchanged.

**The cases asked about, one by one.** Aborted insert: the root is DEAD, becomes LP_DEAD, no bit
until VACUUM has removed the TID. Aborted HOT update: the new version is a DEAD heap-only tuple,
freed; the old one is LIVE (its xmax aborted) and is the one row. HOT chain with committed
updates: redirect to the live member, one row; while the replaced version is still RECENTLY_DEAD
to some snapshot - the reader's included - the page is not all-visible. In-progress insert, update
or delete: INSERT_/DELETE_IN_PROGRESS, no bit (and the writer cleared it anyway). The reader's own
earlier writes: its XID is running, so the same. Frozen tuples: visible to all, one row each.

**The standby.** `heap_page_prune_opt()` returns at once in recovery, so a standby never prunes
on access; it replays the primary's `XLOG_HEAP2_PRUNE_ON_ACCESS` records, whose VM bit is set with
the page state the primary had - no LP_DEAD - and whose `snapshot_conflict_horizon`
(`newest_live_xid`) makes `ResolveRecoveryConflictWithSnapshot()` cancel any standby query that
could still see the page as not visible to all, exactly as for VACUUM's VM records. The LP_DEAD
to LP_UNUSED step is replayed from VACUUM's own records, which follow its index records in WAL
order, and in rmgr mode those take the cleanup locks of §9's standby rule. The argument therefore
reads on the standby word for word; in generic-WAL mode the standby rechecks every TID anyway.

**Conclusion.** The claim holds on 19 and 20devel: on-access VM setting never exposes a TID of a
pinned container that is not exactly one visible row. That covers the pages core cleans on access
today with no change of ours, and the count's own pruning below. Index-only scans depend on the
same property (they too count a TID on an all-visible page without a heap visit), which is some
reassurance, but it is not the argument: the bitmap-heap-scan skip-fetch that core removed in 18
failed on the TIDs a scan holds WITHOUT a pin, which is what §9's pin rule is for.

**The count prunes on access too (`lion_recheck_heap_heap()`, 19+ only).** Before share-locking a
heap block it rechecks, the count calls `heap_page_prune_opt(heap, buf, &vmbuf, rel_read_only)`
once, exactly as `BitmapHeapScanNextBlock()` does: buffer pinned and NOT locked (the
function takes the cleanup lock itself, conditionally, and pins the VM page before trying), the
count's own `vmbuf` pin reused and released where it always was. It takes no lock it could wait
on while holding another: the index pages the merge still holds are pinned, not locked, and the
cleanup lock is conditional. `rel_read_only` is decided when the LionCount node starts, as
`ScanRelIsReadOnly()` decides it for core's scans, but by relation and not by range-table index:
the node's own RTE is never a result relation (it is only planted in a SELECT with no row marks),
so the question is whether the statement modifies or row-locks the counted relation, a partition
of it, or an ancestor of that partition anywhere else - a scalar subquery counting `t` in an
`UPDATE t` must not set bits the UPDATE is about to clear. The SQL-callable counts
(`lion_index_count()` and friends) cannot see their statement and pass false: they prune, as any
core scan of a modified relation does, and never set the VM. The flag is a heuristic about wasted
work only; the argument above does not depend on it.
- *The per-query visibility cache* (§9 step 6) stays valid: pruning only removes versions dead to
  every snapshot, keeps survivors reachable from the root, and never moves a root line pointer,
  which is the argument the cache already rests on (its point 2); a block answered from the cache
  is not read at all and is not pruned.
- *A grouped count* may now meet, in a later group, a page that an earlier group's recheck just
  made all-visible, and count it from the map: that is the claim above, applied to a container
  whose page is pinned at the time of the VM check, like any other.
- *SERIALIZABLE* is unchanged: pruning takes no predicate locks and removes only versions no
  snapshot can see; the tuple locks of the recheck and the page locks of the VM path are what they
  were.
- *The trade*: a count may dirty heap pages and write WAL (a prune record), as core's scans on 19
  already do; it never does so in recovery.

## 12. Measured on 20M rows (2026-09-20) and v1 priorities

Measured (optimized build, see README; final run with sparse segments, inline_limit 4096 and byte-sized
buckets): count pushdown 2.6 ms for a 10M-row key, 56 ms for a 200-group GROUP BY, 4.9 ms for a 3-index
AND; bitmap-scan paths heap-bound at parity with btree/GIN; index build 11-19 s per column; inserts ~4x
btree cost. Sizes on 20M rows: clustered keys 9x smaller than GIN, dense random keys 1.3-2.7x GIN,
sparse high-cardinality keys 1.2-1.3x GIN (c20k 209 MB vs 157, c1m 240 MB vs 199).

v1 priorities, in order of measured impact:
1. DONE (§13): sparse posting representation for keys with few members per container.
2. DONE (§13, "Build policy, measured"): the space policy around it - inline_limit defaulting to
   4096 and bucket counts sized by bytes - which is what turned the saved bytes into a smaller
   index (c20k 164 → 37 MB, c1m 256 → 116 MB).
3. Bitset page packing: a 4104-byte 15-bit bitset fits once per 8152-byte page (c2: 56 MB vs GIN
   21 MB). 14-bit containers pack three per page and shrink c2 to 38 MB but cost mid-cardinality keys
   ~18%; a dedicated two-bitset page format, or splitting only bitset containers, would get both.
   Sharing a container page between keys is the other half of this.
4. Insert cost: a container is copied out and back per insert (bitset fast path exists). Consider an
   in-place add for ARRAY containers with slack and a GIN-style pending list for bulk loads.
5. IN predicates in the AM and the count pushdown are DONE (§15, amsearcharray), and so are NULL
   keys (§14, amsearchnulls) and the multi-key opclasses of §17. Range predicates are §28:
   strategies 6..9 on every ordered scalar class, a bounded walk of the sorted directory (§21) in
   the bitmap scan, and in the count pushdown a range that bounds the entry walk driving the count
   rather than a source of its own.
6. DONE (§18): page recycling and entry deletion.
7. Params in the count pushdown; multi-column GROUP BY. Partitions are DONE (§16).
The per-container visibility-map read (lion_vm_allvisible_mask) is already in: it turned the GROUP BY
from O(heap blocks × keys) (1034 ms) into O(containers) (50 ms).

## 13. Sparse segments (v1 format addition)

Problem (measured, §12): a container costs 12 bytes plus alignment even for one member, so keys with
fewer than ~4 members per 64-page window (high-cardinality columns, tsvector lexemes, c20k/c1m)
cost 24 bytes per row where GIN costs 3.

Item kind. Container pages and INLINE payloads may now hold a fourth item type, the **sparse
segment**, sharing the 8-byte LionContainer header:

    type        = LION_CT_SPARSE (4)
    ckey        = first container key covered by the segment
    cardinality = number of members n (1 .. LION_SPARSE_MAX_PAIRS = 682)
    payload     = uint32 ckeys[n], then uint16 los[n]      -- both sorted by (ckey, lo); 6n bytes
    total size  = 8 + 6n  <= 4104 (same bound as every other item)

A segment covers the ckey range [ckey, ckeys[n-1]]. Invariants: every ckey of a posting set appears
in at most one item (a regular container or inside one segment); items on a page are ordered by first
ckey and their ranges do not overlap or interleave; page minckey/maxckey and chain ordering use each
item's first and last ckey; `lion_item_size()` dispatches on type for containers and segments.
The entry tuple's `ncontainers` counts ITEMS, containers and segments alike (it is the number the
chain machinery maintains and verify() checks against the items it finds); the `containers`,
`array_containers`, `bitset_containers` and `run_containers` columns of lion_index_stats() count
only real containers, and `container_bytes` is the bytes of every item, segments included.

Policy. LION_SPARSE_THRESHOLD = 4: a ckey with ≥ 4 members is a regular container (array cost
12 + 2n beats 6n from n = 4); with ≤ 3 members its pairs live in a segment.
- Build: per key, group sorted codes by ckey; dense ckeys emit containers, sparse ckeys append to the
  open segment; a container closes the open segment (ranges must not interleave); a segment also
  closes at 682 members.
- Insert: locate the item for the ckey (binary search by first ckey; a segment matches when its range
  covers the ckey or when the ckey falls between two items — then prefer appending to the preceding
  segment if it has room and no container lies between, else the following one, else a new 1-member
  segment). Insert the pair in sorted position. If the ckey's members within the segment reach the
  threshold, extract them into a new regular container item and split the segment around it
  (left part, container, right part; empty parts vanish). If a segment overflows 682, split it in
  half. All through lion_chain_put_container-style placement so page splits work unchanged.
  Order matters: the threshold test comes FIRST, because promoting a ckey shrinks the segment, so a
  full segment only has to split in half when the pair really stays in it and one insert can never
  produce more than three items. The half-split therefore only happens for a pair that lands inside
  a full segment's range (out-of-order inserts, e.g. heap pages freed by VACUUM and reused); a pair
  beyond the last item simply starts a new segment when the preceding one is full. A full segment
  all of whose pairs share one ckey cannot be split in half; that state is unreachable under the
  policy, and the insert promotes the ckey to a container instead of failing.
  A key's first TID is a one-pair segment (14 bytes), not a one-member ARRAY container (10): one
  more byte per singleton key, six instead of ten for every ckey after it.
  A container built by promoting a ckey out of a segment gets one `lion_container_optimize()` call,
  exactly as the build path does when it closes a container — that is what makes 200 consecutive
  inserts of one key a 14-byte single-run container instead of a 408-byte array.
- Placement: `lion_chain_put_items_locked()` replaces the item at an offset (or inserts at it) with
  1..3 items in ONE GenericXLog record, splitting the page exactly as before when they do not fit;
  `lion_chain_put_container_locked()` is a one-item wrapper over it. Atomicity matters here: a crash
  between writing a promoted container and the remains of its segment would leave a ckey in two
  items, which every reader would then see twice.
- VACUUM: filter pairs; delete empty segments; a regular container that falls below the threshold
  may stay a container (conversion back is optional).
- Scan: emit pairs grouped by heap block via tbm_add_tuples.
- Count (§9): the set cursor presents each ckey of a segment as a temporary ARRAY container built
  on the fly (≤ 3 members), so the merge/AND/VM-mask code is unchanged; one VM mask read per ckey.
- verify: segments sorted, non-overlapping with neighbours, sizes in range, cardinality == n, each
  ckey inside a segment has < threshold members (after build; inserts may transiently violate only
  until extraction). stats: add sparse_segments and sparse_members columns. "Sizes in range" means
  the item's allocated length is at least what its header needs and at most that plus one slack
  allowance (§4): a segment an insert wrote has room for a few more pairs inside it.
- Container library: `lion_container_check()` rejects type 4 ("item is a sparse segment, not a
  container"); segment helpers live in `src/lion_sparse.[ch]` with their own standalone unit test
  (test/unit/sparse_test.c, `make unit`). Segments never grow while being filtered, so VACUUM's
  regrow path stays container-only.

Expected effect: c20k 475 MB → ~125 MB, c1m 337 MB → ~170 MB (GIN: 157 / 199 MB).

Measured (2026-09-20, implemented; 5M rows, 27072 heap pages, same table before and after):

    column   posting-set bytes   free bytes in those pages   items
    c20k     40.2 MB → 30.1 MB   94.9 MB → 139.8 MB          3.77M containers → 26.9k containers + 46.4k segments
    c1m      49.8 MB → 37.9 MB   170.5 MB → 182.3 MB         4.97M containers → 0 + 993k segments
    c200     unchanged (25 members per ckey: no segment is ever opened)
    c2       unchanged

`pg_relation_size` is the same before and after (c20k 164 MB, c1m 256 MB, c200 12 MB, c2 7272 kB)
because at this scale both keys are already at a page-granularity floor that the item format cannot
move: a CHAIN posting set owns whole container pages (c20k: 20000 keys × 1 page, 156 MB, now 85%
empty), and c1m's entries are INLINE in 32768 bucket pages (256 MB, now 71% empty). The bytes the
format saves therefore only show up as free space until the next two v1 items are done — sharing a
container page between keys, and sizing buckets by bytes rather than by distinct keys. What it
already buys where the posting set can stay inline: c20k with `inline_limit = 4096` is **35 MB**
(GIN 26 MB, btree 34 MB) against 164 MB with the default limit. The write path gains as well: 100k
inserts into a 1M-row table indexed on c20k take 1.64 s → 1.51 s and grow the index by 9.3 MB →
5.1 MB. Counting costs a little: the GROUP BY pushdown over 20000 c20k keys goes 395 ms → 415 ms
(same 3.77M container keys, now built on the fly out of segments); a single-key count is unchanged
at 0.03 ms.

### Build policy, measured (2026-09-20, same 5M-row table, 31848 heap pages)

The two changes of this wave - `inline_limit` defaulting to 4096 instead of 1024, and bucket counts
sized by the bytes the entries need instead of by the number of distinct keys - are what turn the
free space §13 found into a smaller index:

    column   roaring before   roaring after   GIN      btree
    c2            8488 kB         8488 kB     5280 kB  33 MB
    c200            13 MB           14 MB       13 MB  33 MB
    c20k           164 MB           37 MB       26 MB  34 MB
    c1m            256 MB          116 MB       87 MB  56 MB

c20k: 20000 keys × ~1.5 KB of sparse segments each. They used to spill to a container page apiece
(1024-byte limit) - 20000 pages, 85% empty - and now stay inline in 4732 bucket pages across 1807
buckets. c1m: 993308 keys × ~84 bytes, which used to own 32768 bucket head pages because the count
came from ndistinct/32 rounded up to a power of two, and now has 10660 buckets in 14818 pages (the
byte estimate is about 30% low for keys whose members are spread thinly over container keys, so the
buckets overflow by that much; the overflow pages cost nothing but a longer chain). c200 grows by
71 pages: 135 buckets instead of 64. Nothing else moved - c2's two keys own bitset chains either
way, and the posting-set bytes are exactly the ones §13 measured.

## 14. NULL keys (v1, implemented)

One reserved entry per index holds the posting set of rows whose key is NULL: flag
LION_ENTRY_NULLKEY, keylen 0, hash 0, living in bucket 0 and matched by the flag rather than by key.
Everything that searches a bucket for a key skips it (lion_find_entry_ext), and everything that reads
a stored key asks first; `lion_find_null_entry()` is the only way to it. Once located it is an
ordinary entry: it spills to a chain, is filtered by VACUUM and is counted exactly like any other,
and the spill keeps the flag - every path that rewrites an entry's flags (insert spill, VACUUM spill,
lion_entry_rebuild, build) preserves all of LION_ENTRY_RESERVED, and verify() reports a key-less entry
without one as corruption (keylen 0 is not a legal key length).

Build: the tuplesort's key column may be NULL (hash 0 for NULLs; both passes group by the isnull
flag first, then by key). Insert: NULL → the null entry (created on first use). Scan:
`amsearchnulls = true`; SK_SEARCHNULL returns the null entry's set; SK_SEARCHNOTNULL returns every
other entry (a full walk of the index, correct but expensive; the cost model prices it as the whole
index). Stats gain `null_tids`. verify checks there is at most one null entry, that it is in bucket
0 with keylen 0 and hash 0, and that no entry carries unknown flag bits.

Count pushdown as built. The second and third bullets deviate from the sketch this section started
as, both to keep the buffer-pin budget of §9 bounded:

- `col IS NULL` is a positive source: the null entry's posting set, intersected with the rest.
- `col IS NOT NULL` is a NEGATED source, not a union of every other entry. The rows whose key is
  NULL are exactly the members of the null entry, so the answer is the intersection of the other
  clauses MINUS that set, which the merge computes per container key with lion_container_andnot().
  A union of every entry would need one cursor - and up to one buffer pin - per distinct key.
  Any number of `IS NOT NULL` clauses cost one extra cursor each, and no inclusion-exclusion.
- `col IS NOT NULL` with no other clause and no GROUP BY has nothing to drive the merge, so the
  node instead iterates every entry of that column's index and sums the counts (the entries of one
  index are disjoint - a row has one value per column - so the sum is the count of their union;
  §15 states the argument in full). The same disjointness means the clause's own negated source has
  nothing to subtract from any OTHER entry of that index, and nothing to leave of the null entry
  itself, so the driver skips the null entry and drops the source rather than subtracting it at
  every container key of every entry (§15, `st->sumallitem`: 36.9 ms to 13.0 on a 1M-row nullable
  column). A `IS NOT NULL` on a different column keeps its source. EXPLAIN shows this driver as
  `idx (all keys)`, and `EXPLAIN ANALYZE` counts the entries in `Posting Sets Summed`.
- GROUP BY on a nullable column no longer bails: the NULL group is the null entry's count, emitted
  with a NULL datum.
- count(col) is answered when col is declared NOT NULL, or is the group column (count(*) for a real
  group, 0 for the NULL group), or a clause pins it with a strict operator or `IS NOT NULL`
  (count(*)), or a clause says `col IS NULL` (0). The attnotnull requirement of §10 is gone.

## 15. IN lists and ScalarArrayOp (v1, implemented)

AM: `amsearcharray = true`. liongetbitmap receives an SK_SEARCHARRAY key whose argument is an array
datum: deconstruct it, skip NULL elements, look up each value and emit its set. Duplicates are not
filtered there - set semantics make them harmless, they only cost a second lookup.

Count pushdown: accept `ScalarArrayOpExpr` with useOr = true whose operator is strategy 1 of the
index opfamily and whose array is a non-NULL Const of at most LION_MAX_ARRAY_ELEMS = 1000 elements;
the clause's posting set is the UNION of the elements' sets. Values are deduped, and since the
disjoint-sum short-circuit below they are deduped **by entry** and not merely bytewise (see there).
An empty array, an all-NULL one, or a list none of whose values has an entry means
zero rows. Longer lists are left to the ordinary plan, because every listed value needs a posting
set of its own and each may hold a buffer pin for as long as the node runs.

### The disjoint-sum short-circuit

The count of a union is not in general the sum of the counts, but for the posting sets of DIFFERENT
entries of one SCALAR index it is. A scalar opclass extracts exactly one key from a row's column
value, so the build and the insert path put that row's TID under exactly one entry - the entry of
its value, or the reserved NULL entry of §14 - and no TID is therefore in two entries. §14 already
rests on precisely this: `col IS NOT NULL` with nothing else to drive the merge is answered by
summing the counts of every entry of that column's index, and that is only the count of their union
because the entries are disjoint. The short-circuit is the same argument applied to the entries an
IN list names instead of to all of them.

`lion_count_sources_cached()` therefore counts each entry's posting set on its own -
`lion_count_one_set()`, one pass of the ordinary single-key machinery per entry, sharing the
visibility-map pin, the recheck batch and the visibility cache - and adds the results up, whenever

- the IN list is the ONLY positive source (`count(*) WHERE k IN (...)`, the common shape), and
- there is no GROUP BY driver, and
- every set belongs to one index and that index is not multi-key, and
- the sum is the CHEAPER of the two, which is a question about the data rather than about
  correctness and is answered separately below.

**§9 is unchanged and needs no new argument**: the sets are counted one at a time, so at every
container key exactly one leaf cursor is live, it pins the page its container came from, the
visibility map is consulted (`lion_count_container_vm()`) and only then is the cursor advanced.
Nothing is materialised - each set is read once - so the interlock is carried by construction.

Two conditions on the input, both enforced rather than assumed:

- **Multi-key opclasses never take it** (§17). One row yields many keys there, so it is under
  several entries and the sum over them is not a row count - the same reason §17 refuses such an
  index as a GROUP BY or sum-over-all driver. `lion_sources_disjoint_sum()` asks the index
  (`LionState.multikey`) rather than trusting the caller.
- **No two sets may be the SAME entry.** The bytewise dedupe in `lion_posting_set_lookup_many()` is
  not enough: an opclass whose equality is not byte equality - `citext` - has distinct values that
  hash alike and reach one entry, and summing it twice would count its rows twice. Every located
  entry is therefore also compared, by the (page, offset) its entry tuple lives at, with the entries
  the same hash run has already found; `LionPostingSet` carries that pair. The caller states the
  result in `LionCountSource.disjoint`, which only a caller that used that function may set - an IN
  list under an OR (§19) shares one set array with the other leaves and does not.

#### ... and when it is FASTER

Whether the sum is the right answer and whether it is the cheap one are different questions, and
`lion_sum_is_cheaper()` answers the second. Both paths read every container of every set exactly
once and count the same members; what they do not share is where the per-container overheads land.
The SUM asks the visibility map once per (set, container key), because each set is counted on its
own. The MERGE asks it once per container key, for the union, and pays instead a heap sift and a
union step per container. So the sum wins exactly when there is nothing to amortize, and two things
say there is - either one sends the count back to the merge:

- **DENSE sets.** If an entry has many rows at the SAME container key, the merge folds all the sets'
  members at that key into one container and asks the visibility map about it once. Measured at 1M
  rows (15385 heap pages, so 241 container keys), `k IN (1000 values)` with the sum against the same
  binary forced to merge: 20000 distinct keys (1.0 rows per key per container key) 3.6 ms against
  9.9, 5000 keys (1.0) 11.5 against 21.5, 2000 keys (2.1) 25.1 against 32.2, 500 keys (8.3) 21.2
  against 9.0, 200 keys (20.7) 14.0 against 6.5. The crossover is between two and eight rows per
  container key, so `LION_SUM_MAX_DENSITY` is four.
- **MANY of them**, because the merge only becomes good at dense sets once it reaches its bitset
  image (`LION_OR_BITSET_MIN` containers at one key); below that it folds them pairwise, which is
  quadratic in the sets. On the 200-key column above, sum against merge at 5 values is 0.49 ms
  against 0.64, at 15 values 1.14 against 2.37, at 30 values 2.15 against 7.6 - and at 50, where the
  image takes over, 3.5 against 2.5.

The density is read off the entries' own row counts, which the located posting sets carry already
(`ntids`), against the number of container keys the heap has; no extra page is read to decide it.
`lion_cost_count_rel()` makes the same test from the planner's estimates, which is why both
thresholds live in `lion_count.h` rather than in `lion_count.c`.

#### The GROUP BY driver

A GROUP BY on the very column the list constrains takes the short-circuit too, in a different shape:
the listed values ARE the groups, so the node walks the clause's located sets instead of the index's
entries (`lion_next_group_inlist()`) - a thousand sets instead of twenty thousand entries on `c20k`
- and the clause stops being a source of the merge, because a group intersected with the union of a
disjoint list is the group. Any other clause is still intersected with each group, so this is the
group DRIVER half and applies whether or not the sum half does; it is not subject to the density
test above, because a GROUP BY counts each group on its own whatever happens. It is the difference
between a plan and no plan: `SELECT c20k, count(*) ... WHERE c20k IN (1000 values) GROUP BY c20k`
took **about three minutes** when forced before this (182 and 189 s over two runs) - intersecting
all twenty thousand entries with the thousand-way union, which is why the cost model rightly refused
it - and takes 4.6 ms now against the B-tree's 5.1, which it is chosen over.

`EXPLAIN ANALYZE` reports `Posting Sets Summed`: how many posting sets had their counts added into a
total instead of being merged - the entries of a summed IN list, or the entries of a sum-over-all
(§14). Zero means the k-way union below ran, which is what an IN list ANDed with another clause, an
IN list under an OR, a list the density test declined, and every multi-key clause still do. A GROUP
BY reports zero as well: its groups are emitted, not summed.

#### §14's sum-over-all is the same argument

`col IS NOT NULL` with nothing else (§14) sums the counts of every entry of that column's index, and
the reason it may is the one above. The same disjointness also says that the clause's own NEGATED
source - the column's NULL entry, subtracted - has nothing to do: subtracting it from another entry
of the same index removes nothing, and subtracting it from itself leaves nothing, so the driver
skips the NULL entry and drops the source (`st->sumallitem`). That is not bookkeeping: the NULL
entry of a column with many NULLs has a container at every container key, so the merge was running
a `lion_container_andnot()` against a dense bitset at every container key of every entry. A
`IS NOT NULL` on a DIFFERENT column is a different index's set and keeps its source. Measured, 1M
rows, `nullable` (200 values plus 10% NULL): 36.9 ms before, 13.0 ms now, against the sequential
aggregate's 69.1.

The array may also be a Param, or an `ArrayExpr` over literals and Params, which is what `k IN
($1, $2)` and `k = ANY ($1)` keep in a generic plan (§10). The length cap then applies only when the
length is known at plan time - a literal array's and an ArrayExpr's. A Param that IS an array has no
length until the executor has it, and by then there is no plan left to decline in favour of, so it
is answered whatever its length, and the pins are bounded by the lookup instead (below).

### The pin budget (2026-09-23 review)

This section used to say that an unbounded list's pins were "bounded by the index's bucket pages,
since every element's entry lives on one of those". Since §21 that is the number of directory
LEAVES, which grows with the index, and the bound was no bound: with 1.9 kB keys, a few per leaf,
`lion_index_count_any(idx, (SELECT array_agg(k) FROM t))`, the pushdown's `k = ANY ((SELECT
array_agg(k) ...))` and the multicolumn bitmap scan of the same clause all failed with "no unpinned
buffers available" at shared_buffers = 16MB and 9000 keys - and a list one short of that would have
starved every other backend of buffers instead.

So `lion_posting_set_lookup_many()` keeps pins on at most a BUDGET of distinct leaves, and the
budget is the BACKEND's: every list it has located and not yet released draws on the same one, so
two unbounded lists in a query share it rather than taking one each. It is the smaller of
`LION_LOOKUP_MAX_PINS` = 1000, the longest list the planner takes as a literal, and an eighth of
shared_buffers. The eighth is what keeps one backend from exhausting the pool - the failure above
was 9000 leaves against 2048 buffers, where an eighth is 256 and leaves seven eighths to the query's
own heap, visibility-map and chain pages and to every other backend - and it only binds below 64MB,
so on any ordinary configuration a literal list pins exactly the leaves it always did. Pins on the
leaf the previous set already pins cost no buffer and are not counted. Every set that took a new
leaf is marked `budgeted` and returns it when released; a set abandoned by an error never is, so
the count is zeroed at the end of each top-level transaction, and until then it can only be too
high - sets go NOPIN early, which is slower and never wrong. An INLINE set found past the budget
comes out **NOPIN**: its payload copied, its leaf let go, exactly like a materialized set (§9).

The first version of this budget (2fb790e) was this backend's "fair share" of the pool,
`GetAdditionalPinLimit()`, which is NBuffers / MaxBackends: 86 buffers on a stock 128MB,
100-connection server. That sent ordinary queries off the visibility map - a thousand-value literal
`k IN (...) OR x = 1` over a 963-leaf index rechecked 20783 TIDs on 3540 heap blocks (9.9 ms) where
the map answered before - and 18's function returns 0 outright once the share is at most eight,
which made the same version drop the pin of EVERY single lookup (`lion_posting_set_take()`) on a
small pool with many connections, so a plain `k = 5 AND x = 5` over two INLINE sets lost the map.
Single lookups keep their pin unconditionally now. What a caller's loop of them holds is bounded by
the query rather than by the data: a multi-key clause extracts at most `LION_MAX_QUERY_KEYS` = 1000
keys (lion_multikey.c; beyond that the query is answered as ALL and rechecked), so one `@>` or `@@`
clause of the pushdown pins at most 1000 leaves, and a query with many such clauses holds 1000 per
clause - the residual, and the one place a query's text rather than its data sets the number. The
bitmap scan holds none of them past the lookup (below).

A NOPIN set carries no §9 interlock of its own, and the count restores one in each of its shapes:

- **The disjoint sum** (the common case, the list the only positive source). Counting the sets one
  at a time needs only one of them pinned at a time, so `lion_count_one_set()` locates a NOPIN
  set's entry AGAIN, by its stored key, keeps that pin, and counts the fresh copy under it - the
  ordinary §9 order, one set per pass. A list with NOPIN sets takes the sum whatever
  `lion_sum_is_cheaper()` says, because the union would have nothing to carry the interlock.
  Locating again is sound for the reason a chain may be read page by page after its entry was
  found: the entry is the same one (a column has one entry per key), read later under the same
  snapshot, and an entry VACUUM deleted in between held nothing visible to anyone. One count of a
  single set - a group of the §15 GROUP BY driver - does the same.
- **An intersection** (`k = ANY ($1) AND x = 1`): the rule that already lets a set be materialized
  applies unchanged - the count of an intersection may be taken from the visibility map while ONE
  positive source holds a pin at every container key (`lion_source_pinned()`, which now counts a
  NOPIN leaf as unpinned), and the argument on `lion_posting_set_materialize()` is exactly the
  argument for a stale copy. x's set carries it.
- **Nothing carries it** (an OR across columns, §19, whose leaf is a list past the budget; or an
  intersection of sources that are all NOPIN or materialized): the count trusts no visibility map
  (`cx.novm`) and rechecks every candidate in the heap, as a standby with a generic-WAL index does.
  Correct, and slower only for lists over the budget.

The bitmap scan needs no interlock at all - every TID it emits is visited by the executor - so
lion_scan.c drops the pins of its per-key lookups at once (`lion_posting_set_unpin()`), and its IN
lists are budgeted by the lookup like everyone's. test/sql/pinbudget.sql parks a GROUP BY count
with a cursor, whose WHERE sets stay located from the first group to the last, and counts the
index's pinned buffers in pg_buffercache: 1500 before the budget, at most 1000 with it; it proves
every shape above still exact and which of them still answer from the map, and it pins down that a
list within the budget under an OR and a plain AND of two INLINE sets DO answer from the map.

Measured on a stock PostgreSQL 20 server (shared_buffers = 128MB, max_connections = 100), 800000
rows, a 963-page index on `k` (40000 keys, 20 rows each, INLINE), a thousand-value literal list:

| query | 2fb790e (fair-share budget, 86) | now (budget 1000) |
|---|---|---|
| `k IN (1000) OR x = 1` | 9.9 ms, 20783 TIDs rechecked on 3540 blocks | 8.1 ms, all 3540 blocks from the map |
| `k IN (1000)` (disjoint sum) | 4.6 ms | 3.7 ms |

The merge's sources are therefore unions: k sub-cursors merged by container key, yielding the union
of the containers that share the smallest key any of them still has. The §9 pin rule is per
sub-cursor and unchanged - a sub-cursor that contributed to the merged container still pins the page
that container came from, because only the merge advances it, from lion_count_container(), after the
visibility-map checks.

Neither half of that may be done by walking all k sub-cursors, because k is up to 1000:
- **Which sub-cursors are at the smallest key**: a binary MIN-HEAP of the sub-cursors, keyed by
  their current container key. The ones standing at the root's key come off the heap into a "hot"
  list and go back on as they are advanced, so the per-container-key cost is proportional to the
  sub-cursors that take part rather than to k. The heap entry carries a COPY of the child's
  container key and of its container: reading them out of the child while sifting chases a thousand
  ~300-byte cursors in a random access pattern, which measured *slower* than the linear scan it
  replaced (+4 ms on a 1000-value list at 1M rows). Both fields change only when the child is
  advanced, which is also when it is pushed back on, so the copy cannot be stale.
- **The union itself**: folding m containers pairwise builds and re-optimizes m-1 intermediate
  containers, which is quadratic in them. Above `LION_OR_BITSET_MIN` (32) of them at one container
  key, they are ORed into one bitset image of the key's range instead and the result container is
  built from it once. Below that the pairwise fold is cheaper, because the image costs a fixed pass
  over the whole 32768-value range however few members arrive. The threshold is worth a lot in both
  directions: raising it past any list length, so that the fold runs everywhere, took `c20k IN (1000
  values) AND c2 = 1` from 11.1 ms to 21.0 and `c200 IN (1000 values) AND c2 = 1` from 7.6 to
  **263**.
  A third construction was tried and **rejected**, and it is recorded because the shape that
  suggests it is the common one: a union over sparse segments (§13) arrives at every container key
  as a hundred or two throw-away one-member ARRAYs, for which the image's 4 KiB memset and 512-word
  pass look absurd. Gathering those members into one buffer, sorting and writing them out as an
  ARRAY cannot win more than the image's FIXED cost, and that cost is per CONTAINER KEY - the heap
  has only `heap_pages / LION_BLOCKS_PER_CONTAINER` of them, 241 at a million rows, and that count
  grows with the heap exactly as the member count does, so the ratio never improves. It is under a
  millisecond of that eleven-millisecond query. What the query spends its time on is the per-CHILD
  work: 45006 sub-cursor advances and their heap sifts.
- **Locating the entries**: `lion_posting_set_lookup_many()` hashes every value first and then looks
  the entries up in (bucket, hash) order, so the bucket pages are read in ascending block order and
  duplicates - which hash equally and are therefore adjacent in that order - are dropped in one pass
  instead of by comparing every value with every earlier one (half a million `datumIsEqual()` calls
  at 1000 values). `lion_index_count_any(idx, keys)` is the SQL form of the whole path.

**Cost.** A list is priced per element and not per clause (§10's `lion_cost_count_rel()`): one
bucket page each - but read in ascending block order, so at `lion_heap_page_cost()`'s interpolated
page cost rather than at random_page_cost, and shared once the list is longer than the index has
buckets (`Min(nelems, nbuckets)`); one chain page each at seq_page_cost, capped at the container
pages the index actually has; one container step per element per container key; and, when the merge
runs at all, `lion_merge_ops()`. Since §22 the chain pages are what the list really reads: a list
that is one source of an AND and does not drive it is SOUGHT - the merge seeks every sub-cursor of
the union - so each of its k sets is priced by §22's probed bound, and only a list that drives the
merge (or is the only source) pays for all k chains in full. Which one drives is decided from the
MEMBERS, as the executor decides it: a thousand-element list of a high-cardinality column has fewer
members than a dense equality and therefore drives, even though its k sets lie in many more
containers between them. Three things were wrong here and all three refused a query the node
answers several times faster:

- charging a list as a SINGLE bucket page made a thousand-element list look four times cheaper than
  it is, and the planner chose it anyway (the 2026-09-21 follow-up review);
- charging every element a RANDOM read of a one-megabyte index then refused it;
- and charging the merge `members × log2(nelems)`, as if every row were compared its way through the
  heap, asked 24750 of the 26481 cost units for `c200 IN (1000 values)` at 1M rows - a query the
  node answers in 6.3 ms against the B-tree index-only scan's 66 - and refused that.
  `lion_merge_ops()` charges what `lion_ecursor_build()` does instead: a heap sift per CONTAINER,
  log2(k) deep, plus the union of the containers at one key, which is a bitset image per container
  key above `LION_OR_BITSET_MIN` and the members once per fold below it. And when the disjoint sum
  applies there is no merge term at all.

Measured at 1M rows, all-visible heap, warm cache, assert build, `pgbench -M prepared`: the node
with the other scan types disabled, against the same query with
`pg_lion.enable_count_pushdown = off`, which picks a B-tree index-only scan. Unless a row says
otherwise, the planner picks the node on cost with nothing disabled.

| query | lion | B-tree | before (§15 as of 081e067) |
|---|---|---|---|
| `c20k IN (3)` | 0.15 ms | 0.16 | 0.17 |
| `c20k IN (10)` | 0.18 | 0.19 | 0.24 |
| `c20k IN (100)` | 0.50 | 0.50 | 1.49 |
| `c20k IN (1000)` | 3.76 | 3.52 | 10.99 |
| `c20k IN (1000) AND c2 = 1` | 11.14 | 18.77 | 11.50 |
| `c20k IN (1000) GROUP BY c20k` | 4.63 | 5.07 | ~185000 (refused) |
| `t20k IN (1000 texts)` | 3.70 | 18.16 | 10.47 |
| `c200 IN (3)` | 0.34 | 1.13 | 0.43 |
| `c200 IN (10)` | 0.83 | 3.45 | 1.40 |
| `c200 IN (100)` | 3.07 | 34.26 | 3.11 |
| `c200 IN (1000)` | 6.27 | 65.98 | 6.64 (refused) |
| `c2 IN (0, 1)` | 0.89 | 64.73 | 2.57 |
| `nullable IS NOT NULL` | 13.04 | 69.06 | 36.69 |

The two remaining mispredictions are ties the planner resolves the wrong way by a hair: `c20k IN
(1000)` at 1.07x of the B-tree and `c20k IN (3) AND c2 = 1` at 1.11x. `test/sql/pushdown.sql` pins
the choice at 3, 10 and 1000 values on a table of its own, and `test/sql/inlist.sql` at 3, 10, 100
and 1000 on a high- and a low-cardinality column.

Absolute numbers from this build are worth less than the ratios: it is an -O1 assert build, and one
intermediate binary of identical source measured `c20k IN (1000)` at 2.75 ms where every clean
rebuild of it measures 3.8-4.0. Every comparison above is between two runs of the SAME binary.

A GROUP BY over the same column restricts the groups to the listed values (a group outside the list
counts 0 and is not emitted). `col = ANY (...)` with useOr = false (`= ALL`) is not pushed down.
EXPLAIN prints the list as `idx (col = ANY ({1,2,3}))`.

## 16. Partitioned tables (v1, implemented)

At UPPERREL_GROUP_AGG the input rel may be a partitioned parent: `rte->inh`, relkind `p`,
`IS_PARTITIONED_REL()` (part_scheme, boundinfo, nparts > 0, part_rels, not dummy), reloptkind
RELOPT_BASEREL. The parent has no storage and, because `get_relation_info()` skips indexes for an
inheritance parent, no `indexlist` either, so everything is resolved per leaf.

Planning (`lion_try_count_path`, `lion_collect_targets`)
- The query-level, GROUP BY, WHERE-clause and target-list checks of §10/§14/§15 are unchanged and
  are made against the PARENT: the Vars, attnums and constants the node carries are the parent's.
  Clause analysis no longer looks an index up as it goes; it records (attnum, operator, compared
  type) and the indexes are matched afterwards, once per relation.
- `lion_collect_targets()` then walks the partition tree and produces one target - heap Oid, the
  index whose entries drive the scan (GROUP BY or the sum-over-all of §14), one index per WHERE
  clause, and the leaf's RelOptInfo for costing - per live leaf partition. It recurses through
  sub-partitioned children, using the planner's already-pruned set (`part_rels[i]` non-NULL and
  `i` in `live_parts`) and additionally skipping children the planner has proved empty
  (`IS_DUMMY_REL`), which count nothing.
- Column numbers are translated for every child through `root->append_rel_array[childrelid]->
  translated_vars`, since partitions may number their columns differently or have dropped ones. A
  column missing in some partition is a bail-out.
- Every leaf must be a plain heap table (relkind `r`, and a heap by table-AM routine, §2 -
  partitions may each have their own table AM; a materialized view is accepted by the same code
  path but cannot be a partition) with a usable lion index - the rules of
  `lion_find_roaring_index` plus, per clause, strategy 1 of THAT index's opfamily for the clause's
  operator and an opfamily member and hash function for the compared type, all checked per
  partition because nothing stops two partitions from using different opclasses. A foreign table,
  or a leaf without one of the indexes, bails out.
- The two §10 rules about what an index's entries MEAN are checked per partition for the same
  reason: the driving index's strategy-1 operator must be the grouping equality
  (`SortGroupClause.eqop`), and an index whose stored key will be printed - the group key in the
  output, or a column a `=` clause pins - must have the type's own equality and a
  representation-preserving one (`BTEQUALIMAGE_PROC` under that index's collation). One partition
  with a coarser or a differently spelled opclass declines the whole query; there is no per-
  partition fallback, because the node produces one result set.
- No live partitions (everything pruned) adds no path at all: the planner's own dummy-rel handling
  gives the right answer.
- One CustomPath per query - added to the grouped rel directly, or, for a GROUP BY, as the subpath
  of the Finalize Agg that is added instead (below). custom_private gains one member: LION_PRIV_PARTS, a
  list of one OidList per partition (heap Oid, driving index Oid or InvalidOid, then one index Oid
  per WHERE clause in clause order), empty for a plain table. A partitioned plan leaves the index
  Oids in LION_PRIV_OIDS invalid - there is no single index - and keeps the parent's heap Oid there,
  which is what EXPLAIN resolves column names against.
- Cost: `lion_cost_count_rel()` is the per-relation estimate of §10, taking one relation's pages,
  allvisfrac and rows and its own indexes; the path's cost is the sum over the leaves. numgroups
  comes from `estimate_num_groups` on the PARENT (a partition may hold rows of every group), and is
  used unchanged for each of them. Each partition's dirty working set is therefore charged once, for
  the same reason a single table's is (§10): one visibility cache serves the whole node execution
  and empties itself as the partition under it changes, so a partition's dirty pages are fetched
  once per query however many of its groups come back to them (§9). A partition's own
  `effective_cache_size` share is prorated from its own page count, so a partitioned table is not
  quietly treated as one huge relation. Nothing is added for a merge, because there is none, and the
  Finalize Agg on top is costed by core's own `create_agg_path()`/`cost_agg()` - including its
  spill. The node's `rows` is what it really emits: for a partitioned GROUP BY the SUM over the
  partitions of that partition's own `estimate_num_groups` (made against the child Var, so against
  the child's statistics, and capped by its row count), which is also what `cost_agg()` is then
  handed as its input row count.
- **A partitioned GROUP BY emits PARTIAL aggregates; core's Finalize HashAggregate combines them**
  (2026-09-21 follow-up review). This is what bounds the memory, and it replaces the
  cross-partition merge the node used to do itself.

  The node used to hold every distinct group of every partition in a TupleHashTable until the last
  partition had been counted, and that table had no spill path: unlike HashAggregate it could not
  fall back to batches on disk. A plan-time budget guarded it - decline when
  `numgroups × hashentrysize` exceeds `get_hash_memory_limit()` - but a plan-time bound is only as
  good as `estimate_num_groups`, and a partitioned parent is never auto-analyzed. Ten estimated
  groups against twenty thousand actual ones (reproduced: `work_mem = 64kB`, correct output, no
  spill) built a table with no bound at all. So the merge is gone.

  Instead the pushdown builds the plan shape core builds for a partial aggregation:

  - the path's target is a partially-grouped PathTarget made by `lion_make_partial_target()`, which
    is `make_partial_grouping_target()` (src/backend/optimizer/plan/planner.c) applied to the
    grouped rel's own target: the grouping column carried through with its sortgroupref, every
    other column (one a WHERE clause pins, say) carried through as a plain column, and each Aggref
    flat-copied and put through `mark_partial_aggref(AGGSPLIT_INITIAL_SERIAL)`. For `count(*)` and
    the `count(col)` cases of §14 the transition type is int8 and nothing is serialized, so a
    partial row is just (group key, int8 partial count). It has to be *that* function's output,
    node for node: setrefs.c re-derives the partial Aggrefs from the Finalize Agg's own ones
    (`convert_combining_aggrefs()`) and matches them against the subplan's target list with
    `equal()`, so an Aggref differing in any field - the aggsplit above all - would not be found.
    `custom_scan_tlist` is copied from that target list, which is what
    `set_customscan_references()` then resolves the plan's INDEX_VAR references against;
  - and what goes into the grouped rel is not the CustomPath but
    `create_agg_path(root, output_rel, &cpath->path, output_rel->reltarget, AGG_HASHED,
    AGGSPLIT_FINAL_DESERIAL, root->processed_groupClause, NIL, &agg_final_costs, numgroups)`, with
    `agg_final_costs` from `get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, ...)`. EXPLAIN
    therefore reads `Finalize HashAggregate -> Custom Scan (LionCount)`.

  The Finalize node combines the partial counts with `int8pl` and spills to disk under
  `work_mem × hash_mem_multiplier` like any HashAggregate, so a grouping that does not fit is
  batched rather than refused, and a wrong estimate costs performance instead of memory. The node
  itself keeps nothing between rows: one partition open, one entry scan, one group's iteration
  state. Neither a count (no groups) nor a single table (its groups already stream) has anything
  above it. `test/sql/partition.sql` pins the shape with EXPLAIN VERBOSE (`(PARTIAL count(*))` in
  the node's output), shows `batches > 1` / `disk usage > 0` on the Finalize node at
  `work_mem = 64kB` via EXPLAIN (FORMAT JSON), and replays the stale-statistics reproduction above:
  20,010 exact groups out of a plan that estimated 10.
- A partitioned GROUP BY needs a hashable grouping column (`SortGroupClause.hashable`) for the
  Finalize HashAggregate, and needs the planner to consider the aggregates splittable
  (`extra->flags & GROUPING_CAN_PARTIAL_AGG`); one table has nothing above it and cares about
  neither.
- Partitionwise aggregation is still left alone: the `patype != PARTITIONWISE_AGGREGATE_NONE`
  bail-out means that with `enable_partitionwise_aggregate = on` a partitioned table gets the
  planner's own per-partition Aggs and not this node. Two things about that guard are worth
  writing down now that the node produces partials itself:
  - it is wider than its name suggests. `grouping_planner()` sets `extra.patype =
    PARTITIONWISE_AGGREGATE_FULL` whenever the GUC is on and the query has no grouping sets, before
    anything has asked whether the input rel is partitioned at all, so the bail-out currently turns
    the count pushdown off for PLAIN tables too. The GUC defaults to off, so this only bites a
    session that turned it on;
  - and the shape reason for it is gone. The node's output is now exactly what core's own partial
    aggregation produces, so letting the path compete with the partitionwise Aggs on cost would be
    structurally sound; a child level doing `PARTITIONWISE_AGGREGATE_PARTIAL` never reaches the
    hook anyway (`create_ordinary_grouping_paths()` returns before it). Lifting the guard is
    therefore a costing decision, not a correctness one, and wants its own calibration and tests.

Executor
- `lion_open_relation()` / `lion_close_relation()` open and close ONE relation: a plain table once for
  the life of the node, a partition for the length of its own turn. The heap is opened with NoLock -
  the executor holds a lock on every range table entry of the plan, partitions included (the planner
  locked them when it expanded the parent, and `AcquireExecutorLocks()` relocks the whole flat range
  table for a cached plan), and cassert builds check it with `CheckRelationLockedByMe` - and the
  indexes, which are not range table entries, with AccessShareLock.
- Per relation the node then runs one of `lion_count_relation()` (the intersection of the WHERE
  clauses), `lion_sumall_relation()` (§14's sum over every entry) or `lion_next_group()` (§10's
  streaming group loop); all three are the single-table code, unchanged.
- Without GROUP BY the partition counts are summed and one row is emitted, as §10 says: nothing
  comes out until the last partition has been counted, because the one row is the total.
- With GROUP BY the node streams. `lion_next_partial_group()` keeps two pieces of state, which
  partition is open (`curpart`) and whether it is open (`partopen`), and on each call: open the
  partition if it is not open and locate its WHERE clauses; ask `lion_next_group()` for the next
  group of it and return that row; and when its entry scan runs out, release its posting sets,
  close it, and move to the next one. Each group is emitted as a partial aggregate the moment it
  is counted, so a group with rows in three partitions comes out three times and the Finalize Agg
  adds them up. The NULL group is emitted per partition like any other and hash aggregation groups
  NULLs together. Groups whose count is 0 are still not emitted - there is no such group in that
  partition - and a partition where a positive clause has no entry at all (`wheremissing`) is
  skipped without an entry scan. The per-partition work uses the same pergroup context as before,
  reset per group, and the located WHERE payloads are released and their context reset at the end
  of each partition. There is no cross-partition state: no hash table, no per-node group memory
  beyond one partition's iteration state.
- The key a target list prints for a column a clause pins to one value is remembered on the clause
  (`LionClauseState.storedkey`, copied into a small context of its own) the first time a relation's
  entry has it, because the posting set is gone by the time the row comes out. With partitions that
  is the first partition that holds the key; any other partition's key compares equal to it by the
  index's own equality - and, since the planner only builds a value-producing node when every
  partition's index has the type's own representation-preserving equality (§10), "compares equal"
  there means "is the same bytes".
- ReScan throws all of it away - entry scan, posting sets, the open partition and the remembered
  keys - and starts again from the first partition. A Finalize HashAggregate that has not spilled
  re-reads its own table instead of rescanning the node (`ExecReScanAgg()`), so the case that
  really exercises this is a nested loop over a spilling one, which `test/sql/partition.sql` has.
- EXPLAIN prints `Partitions: p1, p2, ...` in the planner's order. The `Lion Indexes` line keeps
  its per-clause text but drops the index name for a partitioned scan (`(a), (b = 2)` rather than
  `idx_a (a), idx_b (b = 2)`), because there is one index per partition and no single name to give.
  `Group Key` and the ANALYZE counters are unchanged and are summed over the partitions.

Locking and the §9 pin discipline are per partition and unchanged. A partition's posting sets are
all released before its indexes are closed, so no partition's index pin outlives its turn, and a
VACUUM of one partition cannot affect the count of another: the interlock argument of §9 is about
one heap and its indexes, and each partition is its own.

Not supported: run-time pruning (the node has no Append and no PartitionPruneInfo, so the partition
set is fixed at plan time), parallel
execution (`flags = 0`, `parallel_safe = false`), and partitionwise aggregation as above. Planning
costs one `index_open` per clause per partition (`lion_index_bucket_pages` reads the meta page), and
EXPLAIN's `Partitions` line names every one of them, so both are linear in the partition count.
Also not supported, and refused rather than attempted: a partition whose opclass groups rows
differently from the query, and a value-producing grouping over a type whose equality does not
preserve the representation (§10). A grouping too large for `hash_mem` is no longer among them:
the Finalize HashAggregate spills.

## 17. Multi-key operator classes: arrays and tsvector (v1, implemented)

GIN-style extraction so one row can contribute many keys: `array_ops` (DEFAULT FOR TYPE anyarray,
STORAGE anyelement) and `tsvector_ops` (DEFAULT FOR TYPE tsvector, STORAGE text).  The entry key type
is the element type / the lexeme; everything below the key - buckets, entries, containers, sparse
segments, VACUUM, the visibility-map interlock - is unchanged.

### Handler and opclass shape

    amsupport = 3     amstrategies = 5     amstorage = true

    proc 1   hash of the KEY type                 (absent when the key type is polymorphic)
    proc 2   GIN extractValue(value, &nkeys, &nullFlags) -> Datum *keys
    proc 3   GIN extractQuery(query, &nkeys, strategy, &pmatch, &extra_data, &nullFlags,
                              &searchMode) -> Datum *keys

    strategies   1 =      2 @>      3 &&      4 <@      5 @@

The extraction functions are GIN's, named directly in `pg_lion--0.1.sql`:

    CREATE OPERATOR CLASS array_ops DEFAULT FOR TYPE anyarray USING lion AS
        OPERATOR 2 @> (anyarray, anyarray),
        OPERATOR 3 && (anyarray, anyarray),
        OPERATOR 4 <@ (anyarray, anyarray),
        FUNCTION 2 ginarrayextract(anyarray, internal, internal),
        FUNCTION 3 ginqueryarrayextract(anyarray, internal, int2, internal, internal, internal, internal),
        STORAGE  anyelement;

    CREATE OPERATOR CLASS tsvector_ops DEFAULT FOR TYPE tsvector USING lion AS
        OPERATOR 5 @@ (tsvector, tsquery),
        FUNCTION 1 hashtext(text),
        FUNCTION 2 gin_extract_tsvector(tsvector, internal, internal),
        FUNCTION 3 gin_extract_tsquery(tsvector, internal, int2, internal, internal, internal, internal),
        STORAGE  text;

GIN's *consistent* function is deliberately NOT reused: it answers "given which keys matched, does
this one row match", and a roaring scan never asks that - it combines whole posting sets.  The query
side therefore builds a boolean tree of its own (below).

Two consequences of borrowing GIN's functions, both binding on any future multi-key opclass:

- `gin_extract_tsquery` is declared `(tsvector, internal, int2, ...)` in pg_proc even though its
  first argument is really a tsquery; that is how GIN declares it (amproclefttype = opcintype), and
  the opclass above copies it verbatim.
- **the strategy number handed to proc 3 is GIN's, not ours.**  GIN numbers the array operators
  1 &&, 2 @>, 3 <@, 4 =, and tsvector's @@ is its strategy 1.  `lion_gin_strategy()` in
  lion_multikey.c is the single place that translation lives.  Getting it wrong is silent - the
  extraction answers for the wrong operator and returns INCLUDE_EMPTY, which only costs a rechecking
  full scan - so it is one function with one switch.

`lionvalidate` has two shapes.  A scalar opclass is checked exactly as before (proc 1 with signature
(T) -> int4, strategy 1 only, every operator's types hashable by the family).  An opclass is
multi-key iff the family has proc 2 for (opcintype, opcintype); then procs 2 and 3 must have GIN's
signatures, every operator's strategy must be in {2,3,4,5}, and proc 1 is required unless
`opckeytype` is polymorphic.  The "every type an operator mentions must be hashable" rule does not
apply to a multi-key family, whose operators mention anyarray and tsquery and whose hashing is of
keys.  `amconsistentequality` stays true: a scalar roaring family holds nothing but equality
operators and a multi-key one holds no equality operator at all, so `equality_ops_are_compatible()`
is never asked about two members of the same roaring family that disagree.

### Key type resolution (`lion_fill_state`, lion_pages.c)

The index's own tuple descriptor already carries the resolved key type: `ConstructTupleDescriptor()`
substitutes `opckeytype` for the column type and replaces ANYELEMENT under an ANYARRAY opcintype with
`get_base_element_type()` of the column.  So `LionState.typid` is still `TupleDescAttr(...)->atttypid`
and a roaring array_ops index on `text[]` has a `text` key column.  (Deviation from the sketch this
section started as, which said "opckeytype when set, else opcintype": taking it from the tuple
descriptor is the same answer for every class, needs no polymorphism handling of its own, and keeps
`enum_ops` - whose opcintype is the pseudo-type anyenum - working as it did.)

- Collation: the index column's (`rd_indcollation[0]`), falling back to **C** when that is invalid
  and the key type is collatable.  A tsvector is not collatable, so a tsvector_ops index has no
  collation to offer, and `hashtext()` refuses to run without one; C is also the right answer,
  because lexemes are byte strings and GIN's own `gin_cmp_tslexeme()` compares them bytewise.
- Hash: support proc 1 when the opclass has one, else the key type's default hash opclass through
  `lookup_type_cache(TYPECACHE_HASH_PROC_FINFO)` - which is how GIN's `initGinState()` resolves its
  comparison function for a polymorphic key type.  ERROR when the type has no hash opclass.
- Equality: strategy 1 of the opfamily for a scalar opclass; for a multi-key one the key type's
  default equality (`TYPECACHE_EQ_OPR_FINFO`), because the family's strategies are about the indexed
  value and not about two keys.  The two always agree: a type's default hash opclass hashes what its
  default btree equality calls equal.

### The reserved EMPTY entry

A second key-less entry joins the NULL entry of §14: flag `LION_ENTRY_EMPTYKEY`, hash 0, keylen 0,
bucket 0, one per index.  It holds the rows a multi-key opclass extracted NO key from - an empty
array, a tsvector with no lexemes - which are under no key at all and which an ALL-mode scan
(`tags @> '{}'`) still has to find.  A NULL column value goes to the NULL entry as before; the two
are mutually exclusive and verify() says so.  `lion_find_entry_ext()` skips both, and
`lion_find_reserved_entry()` (with `lion_find_null_entry()` as a wrapper) is the only way to either.
Like the NULL entry it keeps its flag through a spill, whether the spill happens on insert or inside
VACUUM (§4, §14); test/sql/array.sql and test/sql/tsvector.sql take both transitions.

The meta page version is NOT bumped.  A version 2 index built with a scalar opclass has no empty
entry and needs none - only a multi-key opclass ever writes one, and those did not exist before this
wave - so no existing index answers anything wrongly, which is the test §14 set for a version bump.

`lion_index_stats()` gains `empty_tids`, the member count of that entry, next to `null_tids`.

### Build and insert

Build (`lion_build.c`): the tuplesort tuple grows a fourth column, `kind int2`
(REAL / NULL / EMPTY), still sorted by (hash, code).  A multi-key row is pushed once per distinct
extracted key, all with the same code; both passes group by (kind, key) instead of (isnull, key).
Nothing else changes, because the codes of each key still arrive ascending.

Insert (`lion_insert.c`): `lioninsert()` extracts and then performs one ordinary single-key insert per
key, each taking and releasing its own bucket lock.  They are not atomic with respect to a reader,
which is exactly the visibility the heap already gives: the inserting transaction has not committed,
so no snapshot that can see the row can run before the last of them is written.

Extraction (`lion_extract_value()`, lion_multikey.c) drops NULL keys and duplicates.  A row whose array
holds a NULL element is indexed under its other elements; nothing ever looks for a NULL key, because
a query with one falls back to a rechecking scan.  Duplicates must go, or `lion_container_add()` would
report "already indexed" and leave `ntids` wrong.  The dedupe hashes every key once and sorts the
keys by (hash, the key type's btree order when it has one, §21), so the only candidates for equality
are adjacent, and each key is compared with the equality proc against the distinct keys of its own
run of neighbours that tie on both - one of them.  A row with n keys therefore costs n hashes, one
sort and about 2n comparisons; the keys come out in that order, which nothing depends on.  Sorting
by the hash ALONE made a run every key of one hash, and n distinct keys chosen to share one (int8
`(i << 32) | i`, which `hashint8()` folds to one word) cost n²/2 equality calls: 80000 such
elements took 17 s per INSERT.  A key type with no ordering (xid, cid) still has only the hash, so
there a run is quadratic in the keys that genuinely collide, which the built-in hash functions
keep small.

`ntids` therefore counts (key, row) pairs, and so does `IndexBuildResult.index_tuples`.  A row with
no keys contributes one pair, in the EMPTY entry.  **VACUUM is unchanged**: its callback is per TID
and a TID is removed from every entry that holds it.

verify(heapallindexed) extracts each heap row's keys with the same function the build uses and checks
that the TID is present under every one of them, or in the EMPTY entry when there are none.

### Queries (`lion_extract_query()`, lion_multikey.c)

`searchMode` starts at GIN_SEARCH_MODE_DEFAULT and an out-of-range answer is treated as
GIN_SEARCH_MODE_ALL, as `ginNewScanKey()` does.  The result is one of three modes:

    NONE   nothing matches            DEFAULT mode with zero keys: `tags && '{}'`, an empty tsquery
    KEYS   a boolean tree over the keys, exact
    ALL    every indexed row, with recheck

ALL is the answer for: any searchMode but DEFAULT (INCLUDE_EMPTY and ALL, which is what
`ginqueryarrayextract` returns for `@> '{}'` and for `<@`); any partial-match key (a prefix lexeme -
the keys are hashed, so a range of them cannot be walked); any NULL key; more than
LION_MAX_QUERY_KEYS = 1000 keys; and a tsquery shape the tree builder rejects.

KEYS trees:

- `@>` an AND over every key, `&&` an OR over every key.
- `@@`: the TSQuery's QueryItem array is walked directly.  It is in prefix order - an operator's
  RIGHT operand is at item + 1 and its LEFT at item + qoperator.left (ts_type.h) - and
  `gin_extract_tsquery()` numbers the keys it returns by scanning that array from 0 and counting
  QI_VAL items, so the j'th key is the j'th QI_VAL in array order; the same item -> key map is
  recomputed rather than fished out of extra_data.  QI_VAL with no prefix flag and weight mask 0
  becomes a leaf; OP_AND and OP_OR become AND and OR nodes; **OP_NOT, OP_PHRASE, a prefix and a
  weight mask make the whole query ALL**.  (`!a` has no complement over posting sets that is not
  "every row minus a's", and the ALL fallback is that set anyway; a phrase needs lexeme positions
  and a weight needs weights, neither of which the index stores.)
- `<@` is unreachable (extractQuery says INCLUDE_EMPTY first) and would be ALL.

### Bitmap scans (`lion_scan.c`)

`liongetbitmap` dispatches on `LionState.multikey`.  NONE emits nothing; ALL emits the union of every
entry but the NULL one - the EMPTY entry included - with recheck = true (`lion_emit_all_keys()`, which
is the function `IS NOT NULL` already used); KEYS locates one posting set per key and hands the tree
to the shared evaluator, emitting the result containers with recheck = false.

**The scan reuses the count cursors.**  `lion_sets_iterate()` (lion_count.c) walks
(tree over located posting sets) in ascending container-key order and calls back per container; the
scan's callback is `lion_container_to_tbm()`.  The alternative - one TIDBitmap per key combined with
tbm_intersect/tbm_union - was not needed: the cursors already present a set as one ascending run of
containers, the AND and OR nodes are twenty lines each, and using the same evaluator for the scan and
the count means the two cannot drift apart.

All the posting sets are located BEFORE any of them is walked, so every bucket-page lock the scan
takes is taken before the first container page is pinned: the reader side of the §11 deadlock rule.
`col op ANY (const array)` reaches a multi-key index too (amsearcharray is on for §15's sake); each
element is a query of its own and the answers go into the same bitmap, which is their union.

**Costing the ALL fallback** (`lioncostestimate`, lion_am.c; the 2026-09-21 follow-up review). An
ALL-mode scan reads the WHOLE index and hands the heap every indexed row, and the generic estimate
priced it with the PREDICATE's output selectivity instead - it was handed an empty `GenericCosts` and
told nothing. A prefix query on 200,000 documents was estimated at **192.15** cost units against the
sequential scan's 9,156.14, for a plan that emitted **1,176,746 posting TIDs** and rechecked 198,020
rows across 6,628 heap blocks, and it was chosen by default: **62.3 ms against 23.8 ms**, and the
phrase form **68.6 against 31.8**. So `lioncostestimate` now asks, before calling
`genericcostestimate()`, whether the scan will walk the whole index:
- it picks the qual `liongetbitmap()` would actually answer, by the same ranking that function uses
  (a plain operator first, then a ScalarArrayOp, then a null test), because the cost of the scan is
  the cost of that one qual and the rest are only rechecked;
- `col IS NOT NULL` walks every entry (§14), and so does a multi-key query the extraction above
  answers with LION_QMODE_ALL - which it is asked at plan time, with the clause's own strategy, the
  same way the count pushdown asks;
- a multi-key query whose value is not a plan-time Const is costed as ALL, because the MODE follows
  the query's shape and an unknown shape has to be assumed to be the expensive one;
- then `numIndexTuples` is the index's whole `reltuples` - its (key, row) postings, which is what
  `lionbuild` reports and `lionvacuumcleanup` keeps, though an ANALYZE since will have overwritten it
  with the heap's row count and made this a lower bound - which prorates into every index page, and
  any page the proration still leaves out is added at random_page_cost;
- and for the multi-key fallback ONLY, `indexSelectivity` becomes 1.0, so the bitmap heap scan above
  is costed as a recheck of the whole table. `IS NOT NULL` keeps the clause's own selectivity: the
  rows it emits are exactly the rows it selects, so its heap side is not a full recheck even though
  its index side is a full walk.

Phrase, prefix, weighted and NOT tsqueries, `<@`, `@> '{}'` and a query with a NULL element then lose
the bitmap plan to the sequential scan (measured **61.5/51.9/45.5/65.8 ms** for phrase / prefix /
weight / `<@` against the bitmap plan's 101.3/98.0/88.7/89.1), and to GIN when a GIN index is present
(18.8/7.1/27.3 ms), while exact `@>`, `&&` and AND/OR tsqueries keep the count pushdown and the
bitmap plan unchanged. `test/sql/tsvector.sql` and `test/sql/array.sql` pin the plan shapes.

### The expression evaluator (`lion_count.c`)

§15's union cursor is generalised into `LionExprCursor`, a cursor over an `LionKeyNode` tree whose
leaves are located posting sets.  `LionCountSource` gains a `tree` member; NULL still means "the union
of all the sets", which is what every pre-§17 caller gets.

    LEAF   one LionSetCursor, as before
    OR     stands at the smallest container key any child has left; the container is the OR of the
           children standing there (this is exactly §15's union cursor)
    AND    winds the children forward until they all stand at one container key; the container is
           the AND of theirs, and a container key whose intersection comes out empty is skipped
           here rather than handed up

The §9 pin rule survives both operators: a leaf is only advanced by `lion_ecursor_next()`, and the
merge only calls that from `lion_count_container()`, after the visibility map has been consulted - so
every leaf that contributed to a counted container still pins the page that container came from.
The two places an AND lets a pin go without anything having been counted (winding a laggard forward,
dropping an empty intersection) are safe for the same reason the merge's own `!alleq` branch is:
nothing of that container key reaches the visibility map.

Materialization (§9) needed a sharper rule, because with a tree "at least one participating set is
read the pinned way" is no longer implied by counting pinned sets.  `lion_source_pinned()` decides,
per source, whether EVERY container it can yield comes with a live pin: a leaf has it unless its set
is materialized, an AND has it if ANY child has it (all children stand at the key), an OR only if
EVERY child has it (which children contributed is not known in advance).  A positive source's set is
only materialized while some positive source still has it.

### Count pushdown (`lion_customscan.c`)

A new clause kind, `LION_CLAUSE_MULTI`: an OpExpr whose operator is strategy 2, 3 or 5 of some roaring
opfamily, with the column on the LEFT (these operators do not commute: `'{a}' @> tags` is strategy 4)
and a non-NULL Const on the right.  Strategy 4 and everything that is not a roaring operator bail.

- The strategy is read from the OPERATOR (`lion_op_roaring_strategy()`, a pg_amop lookup restricted to
  the roaring AM), not from an index, because the parent of a partitioned table has no index list
  (§16) and the clause kind has to be known before any index is matched.  `lion_match_index()` then
  re-checks the strategy against the index that will really answer the clause, per partition.
- The query is extracted AT PLAN TIME and the clause is only pushed down when the mode is KEYS.  An
  ALL-mode query would have every row rechecked in the heap, which is what the ordinary bitmap plan
  already does, better.  `lion_match_index()` additionally insists that each relation's index carries
  the very extractQuery function the plan-time extraction used, so the run-time extraction cannot
  come out differently.
- **A multi-key clause therefore requires a Const**, even though §10 accepts a Param for equality
  and for an IN list.  What decides whether this node can answer the clause at all is the query's
  SHAPE - `tags @> $1` with `$1 = '{}'` extracts to ALL mode, as does a phrase or a prefix tsquery -
  and a Param has no shape until the executor has it, at which point the plan is fixed and there is
  nothing to fall back to: the node cannot recheck the operator against the heap, so it would have
  to error on a query it was handed legitimately.  A generic plan over `tags @> $1` therefore uses
  the ordinary plan (costed as ALL, above); a custom plan folds the parameter to a literal and is
  pushed down as usual.  `test/sql/array.sql` pins both.
- At run time the clause's source is the tree over its keys' posting sets, ANDed with the other
  clauses by the merge, exactly like an IN list's union.  Several multi-key clauses on ONE column are
  allowed (the one-positive-clause-per-column rule of §10 is for clauses that pin a value;
  `tags @> '{a}' AND tags && '{b,c}'` is just an AND of three key sets).
- A multi-key clause pins no value, so the column it constrains cannot be printed by the target list;
  it does make the column non-null, so `count(col)` is still answerable.
- **A multi-key index can never DRIVE a count.**  Its entries are keys, not column values, so
  neither §10's GROUP BY (the groups would be lexemes) nor §14's sum-over-all (a row appears under
  each of its keys, so the sum of the entries is not the number of rows) is correct.
  `lion_find_roaring_index()` takes a `multikey` flag and the driving lookup passes false, which makes
  `count(*) WHERE tags IS NOT NULL` fall back to the ordinary plan.  GROUP BY on a scalar roaring
  column next to a multi-key WHERE clause is the supported and tested combination.
- EXPLAIN prints the clause with its operator: `Lion Indexes: idx (tags @> {t5,t7})`,
  `idx (tsv @@ 'w1' & 'w2')`.

### Cardinality guard

Reloption `max_entries` (int, default 0 = unlimited, ShareUpdateExclusiveLock).  Exceeding it is a
WARNING, once per backend per index (a static HTAB keyed by relation Oid), and never rejects a row.

- Build: exact.  Pass 1 has just counted the distinct keys, so the test is `ndistinct > max_entries`.
- Insert: a running estimate, and only on the one path that can add a key - creating a new entry.
  **The heuristic is `entries on this bucket's chain + 1, times the bucket count`.**  Hashes spread
  keys evenly enough for the product to have the right order of magnitude, and the bucket chain is
  already locked and about to be walked anyway, so the guard costs nothing on the hot path.  It is
  deliberately crude: counting the whole index per new key would read every bucket page.

### Not supported

`<@` from the posting sets (a row matches when it has no key OUTSIDE the query array, which the index
cannot tell); prefix, phrase and weighted tsqueries from the posting sets; `col op ANY (...)` in the
count pushdown (only in the bitmap scan); a Param as a multi-key query in the count pushdown (above);
a multi-key index as the GROUP BY or sum-over-all driver;
`lion_index_count(idx, key)` on a multi-key index (it needs a strategy-1 operator and errors out).

### Measured (2026-09-20, 1M rows, 521 MB heap, all-visible, warm cache)

`tags text[]` of 5 tags out of 1000; `tsv tsvector` of 30 lexemes out of 20000 (4.99M and 29.98M
(key, row) pairs).

    column   roaring build   roaring size   GIN build   GIN size
    tags          18.4 s         37 MB        2.9 s      23 MB
    tsv          107.6 s        390 MB       19.3 s     157 MB

The posting-set BYTES are competitive - 20.4 MB for tags and 177 MB for tsv against GIN's 23 and 157
- and the size difference is entirely the page-granularity floor of §12 item 3: every one of the
1000 / 20000 keys is too big to stay inline (5000 and 1500 members) and so owns whole container
pages, 4 and 2 of them, leaving 10.8 MB and 214 MB of free space inside them.  Sharing a container
page between keys is what would close that gap, and it is the same item that was already the top
open one before this section.  The build time is the second half of the same story: 30M (key, row)
pairs go through one tuplesort and then into 40000 pages that are each written once.

    count(*) WHERE ...                     rows   pushdown   roaring bitmap+Agg   GIN bitmap+Agg
    tags @> '{t17,t42}'                      16    0.14 ms        0.39 ms            0.72 ms
    tags && '{t17,t42}'                   10031    0.38 ms        7.6 ms             8.0 ms
    tsv  @@ to_tsquery('w17 & w42')            1    0.09 ms        0.11 ms            0.23 ms
    tsv  @@ to_tsquery('w17 | w42')         2915    0.21 ms        2.3 ms             2.3 ms

The bitmap paths are at parity with GIN, heap-bound as §12 measured for scalar keys.  What the
multi-key classes are for is the first column: the pushdown answers the 10031-row `&&` in 0.38 ms
against 7.6 ms, because it never visits the heap - `Heap Blocks Skipped via VM: 9358, Heap TIDs
Rechecked: 0, Containers Visited: 2071`.

## 18. VACUUM cost, entry deletion, and page reuse (format version 3)

Measured problems (bench/COMPARISON.md, bench/results/2026-09-21-stress): a VACUUM over the 1M-row,
eight-index portfolio takes 1,141 ms and 316 MiB of WAL against btree's 380 ms / 137 MiB; in the
changing-key churn test the fifth-cycle VACUUM takes 1,017 ms against btree's 41 ms, 600,000 historical
entries remain for 100,000 live rows, and the two-index portfolio grows from 7.6 to 34 MiB until REINDEX.

**Measured first** (bench/vacuum_micro.sh, assert build, relative numbers only; the raw runs are in
bench/results/vacuum-micro-before and bench/results/vacuum-micro-after).  ambulkdelete reports its own breakdown at DEBUG1 -
cleanup-lock waits, container filtering, GenericXLog apply, INLINE entry work, pages visited/changed,
records, items rewritten/deleted - and pg_walinspect splits the WAL by resource manager.  On the
1M-row eight-index portfolio with 1% of the rows deleted, the index side of the VACUUM was 696 ms of
the 1,050 ms total, and it went:

    INLINE entry handling                423 ms (61%)   ix_c1m alone 278 ms
    container filtering (the callback)    96 ms (14%)
    GenericXLog apply                     30 ms ( 4%)
    bucket/chain page walking            147 ms (21%)
    cleanup-lock waits                   1.0 ms

with 82 MiB of generic WAL (49 MiB of it full-page images) in **20,222 records** against btree's
58 MiB in 8,312.  Two things stood out and shaped everything below.

1. *The INLINE work was almost all wasted.*  A VACUUM that removes 1% of the rows leaves 99% of the
   entries unchanged, and the old code found that out only after copying the payload out, filtering
   it into a StringInfo and building a new entry tuple.  632,130 entries × ~400 ns of allocation is
   the 278 ms.  So the filter now runs a read-only PROBE first, straight out of the page (the
   cleanup lock makes it stable) into the shared work buffer, and only an entry that really loses a
   member pays for the second pass that builds its payload.
2. *One WAL record per changed ENTRY, each carrying the whole tail of its page.*  20,222 records for
   1,422 changed container pages: the rest were INLINE entries, one `lion_replace_entry()` record
   each, and because the entry shrank, PageIndexTupleOverwrite() moved every entry after it and the
   GenericXLog delta covered all of that - 33 MiB of deltas for a few hundred KiB of real change.
   Hence the two rules below: an entry keeps its bytes, and a bucket page's entries are written in
   ONE record.

**Shrink in place.** Removing members from an ARRAY/RUN/segment item must not move the rest of the
page: the item keeps the bytes the page has allotted it and the freed bytes become slack (§4 growth
slack), so PageIndexTupleOverwrite() moves nothing and the GenericXLog delta is the item alone.  An
item that has lost more than `LION_ITEM_SLACK_VACUUM` = 256 bytes gives the space back and pays the
page-tail delta once; `LION_ITEM_SLACK_BOUND` is the larger of that and the insert path's bound, and
is what verify() accepts and what lion_index_stats() reports as `slack_bytes`.  A page whose filter
removes nothing is never registered for WAL (already the rule).  The same applies to an INLINE
entry's payload on a bucket page: a filtered payload is written back into the entry's existing bytes
with the remainder ZEROED, up to `LION_ENTRY_SLACK_BOUND` = 256 bytes.  No length field was needed
for that - no item kind has type 0, so `lion_inline_fetch()` stops at the first zero item header and
the padding is self-describing; verify() bounds it and checks that every byte of it really is zero.

**One record per bucket page.** Pass 1 collects every INLINE entry of a bucket page that changed and
writes them all in a single GenericXLog record.  Entry offsets survive PageIndexTupleOverwrite(), so
the writes do not disturb each other.  A payload that outgrew its entry still spills onto container
pages in a record of its own (rare: it needs a RUN container to become a BITSET while losing
members).

**Entry deletion.** An entry with ntids = 0 and ncontainers = 0 is deleted by VACUUM:
- The deletion happens in a final step for that bucket page, after every chain of that page's
  entries has been walked, under a cleanup lock, with all of the page's dead entries in ONE record.
  Emptiness is re-decided *there*, not from what pass 1 or pass 2 saw: an insert may have put a TID
  back, and then the entry stays.  An INLINE entry that pass 1 emptied is still written back empty
  first - its dead TIDs have to leave the page under that cleanup lock - and deleted afterwards.
  Reserved NULL/EMPTY entries are deleted like any other; inserts recreate them on demand.
- **Entry offsets on a bucket page never change.**  This is the invariant readers rely on, and it is
  what `lion_delete_entries()` buys by going through `PageIndexTupleDeleteNoCompact()` instead of
  `PageIndexMultiDelete()`: the item's bytes are freed but the line pointer array is left alone, the
  deleted entry shows up as an unused line pointer that every walk already skips, and nothing else
  moves.  The freed line pointers are marked so that the next `lion_add_entry()` reuses them.
  *(Deviation from the first draft of this section, which had readers copy the whole bucket page
  under a SHARE lock and iterate the copy.  Offset stability is strictly cheaper - no BLCKSZ memcpy
  per page per reader - and it is the only thing the copy was for: `lion_entry_scan_next()` gives up
  its lock between two entries of one page and comes back to an offset.  An INLINE posting set still
  keeps its bucket page PINNED until its containers have been through the visibility-map check, as
  §9 requires; that was always separate from the copy.  It also needed no change to
  LionPostingSet/LionEntryScan, which live in a header this wave does not own.)*
- A group cannot come back twice by the other route either - the entry deleted and the key inserted
  again further along the page - because only an EMPTY posting set is deleted, and a posting set
  whose members are all dead to every snapshot has nothing the scan's snapshot could have counted
  before.  test/isolation/vacuum_entry_delete.spec parks a one-column and a two-column GROUP BY
  between two entries of a bucket page while a VACUUM deletes half of them.
- Inserts are unaffected: they hold the bucket page EXCLUSIVE for the whole insert and look entries
  up by key.

**Whole-chain free.** When a CHAIN entry is deleted its chain is freed: (1) delete the entry (one
record; from here the chain is unreachable for new readers and inserts), then (2) for each chain
page, under ConditionalLockBufferForCleanup (skip the page on failure; it is merely leaked, and the
walk stops because the rightlink cannot be read without the lock), check that the page still claims
this chain and is empty, mark it LION_PAGE_DELETED, store `safexid = ReadNextFullTransactionId()` in
the page body (as nbtree's BTDeletedPageData), WAL-log, and RecordFreeIndexPage().  Step 2 runs with
the bucket page unlocked, because VACUUM never waits for a page while holding another LWLock (§11).
A crash between (1) and (2) leaks pages, which is harmless: they are unreferenced and the sweep
below finds them (test/recovery/run.sh phase 1b puts the crash exactly there).  Mid-chain unlinking
of empty pages of a live key is NOT part of this version (documented limitation: a key whose oldest
pages empty out keeps them until REINDEX).

**Reuse.** *(§22 adds one thing to the sweep below: a leaked posting set's ROOT is an INTERNAL page
with downlinks on it, not an empty one, so the sweep runs in ROUNDS - an unreferenced internal
posting page is freed once every one of its downlinks names a DELETED page, which the round before
made true of its leaves. A LIVE internal page can never pass that test: every leaf of a live posting
set was visited by pass 2 and is therefore never swept, and a page a concurrent insert created since
then is full of the items a split just put there. verify() reports such a page as the ordinary leak
it is, with a WARNING.)*

`lion_new_buffer()`/`lion_alloc_page()` (§25 split the second out of `lion_new_buffer_xl()`) first try GetFreeIndexPage(): take the page with
ConditionalLockBuffer; it is reusable only if it is all-zero or LION_PAGE_DELETED with
GlobalVisCheckRemovableFullXid(heaprel, safexid) true (the nbtree rule: no scan that could still hold
a link to it is running); otherwise re-record it in the FSM and extend the relation (which also keeps
the loop from spinning on a block the map keeps offering).  heaprel comes from `heapRel` in aminsert
and `info->heaprel` in VACUUM; a NULL heaprel means "never recycle", which is what ambuildempty
wants.  ambuild never reuses.  amvacuumcleanup calls IndexFreeSpaceMapVacuum().  Leak recovery:
ambulkdelete keeps a bitmap of the blocks it accounted for - the meta page, every bucket page it
walked, every container page it reached from a live entry, minus the ones it freed - and sweeps the
rest at the end of ambulkdelete: a DELETED page or an all-zero page goes into the FSM, and an
unreferenced EMPTY container page becomes a DELETED one first.  In a healthy index nothing is
unaccounted for, so the sweep reads no pages at all.  An empty container page of a LIVE chain is
never mistaken for a leak, because pass 2 visits every page of every chain it found and a chain
created after pass 1 read its bucket page has no empty page in it (a spill and a split both fill
every page they allocate inside the record that allocates it, under the lock they hold throughout).
stats report deleted_pages; pages_newly_deleted/pages_deleted/pages_free are reported to VACUUM.

**Hot standby.** Generic WAL cannot raise recovery conflicts - nbtree's XLOG_BTREE_REUSE_PAGE has no
equivalent - so on a standby a reader holding a stale chain link could land on a page that replay has
already reused.  Every container page therefore carries its owner in the special area, and readers
validate it:

    typedef struct LionPageOpaqueData {       /* version 3: 24 bytes */
        BlockNumber rightlink; uint32 minckey; uint32 maxckey;
        uint32      owner_hash;   /* hash of the entry this chain belongs to (0 for reserved entries) */
        BlockNumber owner_head;   /* head block of that chain, fixed for the life of the chain */
        uint16 flags; uint16 page_id; }

Every reader that reaches a container page through an entry's head or a rightlink (scan, count
cursors, insert, VACUUM, verify) checks: CONTAINER flag set, DELETED clear, and the owner equal to
the entry's.  A mismatch means the chain was freed after the reader copied the entry, which can only
happen when the posting set held nothing visible to anyone: the reader treats it as the end of the
chain (a count contributes nothing further).  It is never an error outside verify(), except on the
paths that hold the entry's bucket page throughout - insert and VACUUM's own walk - where the chain
cannot go away and a mismatch is corruption.

**owner_head alone identifies a chain**, for the whole life of the index, and that is deliberate: a
chain's HEAD page is the one page `lion_new_buffer()` never takes from the free space map
(`lion_entry_spill()` passes reuse = false), so a head block always comes from extending the
relation, is a block number that has never been handed out before, and is therefore never reused as
a head. Since §22 the head is the posting tree's ROOT rather than its first leaf, and the rule is
unchanged because a root split is a PUSH-DOWN that keeps the root at its block: `head` never moves
for the life of the set, so owner_head is still an identity nothing else can collide with.  A reused page always belongs to some other chain, whose head block differs.  *(Deviation
from the first draft, which relied on the pair (owner_hash, owner_head) and accepted "a simultaneous
32-bit hash and head-block coincidence".  The reason for the change is not the coincidence but
reachability: the count cursor's LionPostingSet carries only the head block, not the entry's hash,
and LionPostingSet lives in a header this wave does not own - so making owner_head sufficient is
what lets the count path validate at all.  It is also strictly stronger than the pair check, since
the freed head block is exactly the block the FSM is most likely to hand back.  The cost is that a
freed head block can only ever come back as a non-head page.)*  owner_hash stays as the second half
of the check for the readers that do have the entry in hand, and is what lets verify() prove that a
chain belongs to its key.

LION_VERSION becomes 3; older indexes are refused with the existing REINDEX hint.  The bump is
unavoidable and not merely advisory this time: the special area grows from 16 to 24 bytes, which
moves every item on every page.

**Measured after** (same script, same cluster; bench/results/vacuum-micro-before and bench/results/vacuum-micro-after).  The
78 MiB of XLOG/FPI_FOR_HINT in the WAL column is the heap's own, identical on both sides, so the
column that matters is the index's:

    portfolio, 1M rows, 8 indexes, DELETE id % 100 = 1, one VACUUM
                          VACUUM ms   total WAL   index WAL (of which FPI)   index records
      before (lion)          1050      162 MiB     82 MiB (49 MiB)              20,222
      after  (lion)           728      138 MiB     58 MiB (49 MiB)              10,198
      btree (control)         585      138 MiB     58 MiB (58 MiB)               8,311

    ... of which ambulkdelete itself (sum of the DEBUG1 breakdowns)
                          total    inline   filter   apply   walk   cleanup-lock wait
      before                696 ms   423      96       30     147     1.0
      after                 418 ms   186      89       28     115     0.6

    churn, 100k rows, unique bigint key + two-valued key, UPDATE every key then VACUUM
                       cycle 1                          cycle 5
                   ms      WAL     size   entries    ms      WAL     size   entries
      before      1343    428 MiB  14 MiB  200,000   1456   379 MiB  34 MiB  600,000
      after        135    4.6 MiB  14 MiB  100,000    140   6.2 MiB  15 MiB  100,000
      btree         58    3.3 MiB 5.8 MiB     -        60   3.3 MiB  10 MiB     -

The index WAL of the portfolio VACUUM is now exactly at btree's, and all but 9 MiB of it is
full-page images that the first touch of a page after a checkpoint costs whatever the AM is; the
deltas went from 33 MiB to 9 MiB.  The churn index no longer grows: entries stay at one per live row
instead of accumulating a cycle of history each time, the VACUUM is ten times faster and writes
sixty times less WAL, and the pages the dead keys owned are handed straight back through the free
space map (deleted_pages is 0 at the end of every cycle because the next cycle's inserts have
already taken them).  What is left between lion and btree on the portfolio is ix_c1m, whose 628,508
single-TID entries cost 196 ms of the 418: probing 990,000 dead-TID lookups in hash order, one
bucket page at a time.

**The §9/§11 proof is unchanged**: TIDs are still only removed under a cleanup lock taken in chain
order on every page; a page is freed only after it has been cleaned and found empty under that
lock, so a reader's pin on a page it took containers from still blocks everything that could make
those TIDs' heap pages all-visible.

### Measured, 2026-09-23: parallel VACUUM, and the callback asked once per TID

The focused benchmark of 2026-09-23 (PostgreSQL 18.6, release build,
bench/results/2026-09-23-eb1579e-pg18-focused) had `VACUUM (ANALYZE)` of the 5M-row, seven-index
scalar table after its maintenance sequence at B-tree 3,399 ms, GIN 3,823 ms and lion 4,374 ms. **Most of that gap was not lion's code at all: lion
declared `VACUUM_OPTION_NO_PARALLEL`**, and the benchmark runs VACUUM with the default
`max_parallel_maintenance_workers = 2`, so B-tree's seven indexes were vacuumed by three processes and
lion's by one. Measured separately (below), lion's serial VACUUM was 14-20% slower than B-tree's
serial one, and its default (parallel for B-tree only) one 38-47% slower.

*Set-up* (release builds, -O2 and no assertions, PostgreSQL 19beta4; shared_buffers 512MB,
maintenance_work_mem 512MB, max_wal_size 8GB, checkpoint_timeout 1h, fsync and synchronous_commit on,
autovacuum off, max_parallel_maintenance_workers left at 2 - the benchmark's settings). Two clusters,
one without and one with `shared_preload_libraries = 'pg_lion'`, so every index is generic-WAL in
the first and rmgr-WAL in the second (§25). One run is: `CREATE TABLE fact AS SELECT * FROM fact0`
(fact0 is the benchmark generator's table, `bench/comprehensive/workloads.py` scalar_data(), built
once), `VACUUM (FREEZE, ANALYZE) fact`, the seven single-column indexes of ONE family (c2, c20,
c200, c20k, c1m, skew, nullable; `none` for the heap alone), the benchmark's maintenance sequence
(`INSERT` of 1% new rows, `UPDATE fact SET c200 = (c200 + 1) % 200 WHERE id <= rows/100`,
`DELETE FROM fact WHERE id % 100 = 1`), `CHECKPOINT`, and then the timed statement, `VACUUM fact` or
`VACUUM (PARALLEL 0) fact`, with `client_min_messages = debug1` to collect ambulkdelete's own
breakdown. The old and the new `pg_lion.so` were swapped (rename, restart) between every round, the
configurations alternate inside a round, and the machine was shared with other work, so each cell is
a median with its [min-max] and anything under 10% is noise. (The driver was a throwaway shell
script around exactly these statements and is not in the tree; bench/vacuum_micro.sh measures the
older eight-index portfolio above.)

*Where the time goes* (5M rows, rmgr, serial, old code; ms, sum over the seven indexes):
1,224 ms of ambulkdelete, of which chain-page filtering 400, INLINE entry work 618, WAL apply 94,
cleanup-lock waits 1, and the walk itself the rest. Filtering is almost all the dead-TID callback:
c2's 5M TIDs filter in 82 ms, 16 ns a TID, and every index has 5M TIDs, so the callback alone is
about half of ambulkdelete. nbtree asks the same callback about the same TIDs, and the API gives no
cheaper question to ask (it is opaque; a range test against the dead-TID store would need the
store, which is not the AM's), so that half is the floor. The INLINE work had one real waste: an
entry that changes was filtered TWICE, a probe pass to find out whether anything changes and then a
second pass - asking the callback again - to build the new payload, and on c20k (twenty thousand
entries of 250 TIDs) every entry changes. A cycle-counter split of pass 1 (temporary, on the new
code) shows what is left: on c1m (a million entries of five TIDs) 40% is filtering the unchanged
entries, 41% writing the changed leaves - one record per leaf, each with its full-page image after
the checkpoint, the same as nbtree pays per leaf - and 9% the 95k entries that change; on c20k 62%
is filtering the entries that change, now once, and 30% writing them.

What changed:

1. **`amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL`** (§6, `lion_am.c`). §11 has the
   argument for why the interlock holds whatever process runs an index's ambulkdelete;
   `test/isolation/vacuum_parallel.spec` makes a worker run one (it reports `f` with the old
   setting) and checks the result. amvacuumcleanup stays with the leader: it only vacuums the FSM.
2. **One pass over an INLINE payload**: the callback is asked about every member once
   (`lion_vacuum_inline_filter()`). The items in front of the first one that loses a member are
   copied from the page when it turns up; `lion_container_optimize()` is a function of the member
   set, so the payload built is byte for byte the old one (`test/sql/vacuum.sql`, "INLINE entries
   whose FIRST items lose nothing", drives the prefix copy through ARRAY, RUN and sparse items).
3. An INLINE entry that is not about to be deleted is not kept past pass 1 and its key is not
   copied: one allocation less per entry. Not measurable on its own.

Neither protocol nor WAL changed: the WAL of every run is identical before and after (the column).

    VACUUM wall ms, median [min-max]       old lion              new lion              btree          WAL MiB (lion / btree)
    1M rows (5 rounds)
      rmgr,    default (parallel)       561 [492-619]         471 [451-501]         370-392 [354-454]   139 / 136
      generic, default (parallel)       658 [640-694]         505 [497-552]            -                147
      rmgr,    PARALLEL 0               596 [568-651]         537 [520-596]         483-504 [452-544]
      generic, PARALLEL 0               654 [590-718]         662 [611-2027]           -
      heap alone (no index)             301-329 [275-375]
    5M rows (3 rounds)
      rmgr,    default (parallel)     3,350 [3061-4012]     2,638 [2568-2794]     2,314-2,544 [2223-3628]  579 / 644
      generic, default (parallel)     3,347 [3283-4027]     2,648 [2551-3062]          -                580
      rmgr,    PARALLEL 0             3,391 [2991-3466]     3,775 [3422-4837]*    2,888-2,983 [2791-4800]
      heap alone (no index)           1,781 [1650-3370]     (1,509 min)

    ambulkdelete, sum over the seven indexes (serial runs), median ms
                     old rmgr   new rmgr   old generic   new generic     of which c20k (rmgr)
      1M rows          293        262          372           350             44 -> 32
      5M rows        1,224      1,143        1,329         1,256            278 -> 200

(*) the new serial 5M cell and the second heap-alone cell were hit by other work on the machine -
their minima are in line with the old ones; the ambulkdelete sums, which only time the index, went
down. A B-tree cell gives two medians: its runs alternated with the old and with the new lion build,
and it is the same B-tree both times.

So the VACUUM of the benchmark's portfolio is now 16-23% faster in both WAL modes: 4-14% slower
than B-tree's at 5M rows (from 38%), 20-27% at 1M (from 47%). What is left is shape, not overhead:
with three participants the wall time is bounded below by the longest index, and c1m - 632k INLINE
entries at 1M rows - is that index there (133 ms of the parallel run's 471). Its cost is one callback per TID, one pass over each
entry and one full-page image per leaf, which is what nbtree pays for the same column. Not done:
the REGROW path (§18, `lion_vacuum_regrow()`) filters a container that outgrew its slot a second
time - on skew, 561 containers at 5M rows, about 30 ms of that index's 128 - and could instead reuse
the first result when the container's bytes on the page are unchanged; below the noise here, and
it touches the one path that re-places containers, so it waits for a workload where it matters.

## 19. OR across columns in the count pushdown (v1, implemented)

`count(*) WHERE c200 = 17 OR c20 = 3` used to be declined and ran as BitmapOr + Bitmap Heap Scan
(19 ms at 1M rows on the optimized build, the same as btree, because 5% selectivity touches nearly
every heap page). The count evaluator already evaluates AND/OR trees over posting-set cursors
(multi-key classes, IN lists), so the whole of this is a planner and a plumbing problem.

**Planner.** A top-level `BoolExpr OR` in baserestrictinfo is accepted when every arm is an accepted
positive clause (equality, IN, IS NULL, exact multi-key) or an `AND` of such clauses, each on a plain
column of the same relation; the collation, opfamily, cross-type, Param, IN-list-cap and
per-partition rules are exactly the ones a plain clause goes through, because the analysis IS the
same function (`lion_analyze_leaf()`, with `allow_negated = false` under an OR). A negated arm
(`IS NOT NULL`, `NOT`) is declined: the complement of a posting set is not a posting set, and under a
union there is nothing to subtract it from. So is an arm that is not indexable, one that would need a
recheck (`b > 3`, a non-roaring operator, an ALL-mode multi-key query), and an `AND` arm one of whose
leaves is any of those.

The OR is ONE count source whose expression tree has leaves in different indexes, and it is ANDed
with the remaining clauses and with the GROUP BY driver exactly like any other source. Several OR
restrictions in one query are several sources.

**How it is carried.** The leaves are appended to the flattened clause array alongside the plain
clauses, contiguous and in arm order, and the structure over them travels in a new `custom_private`
member, `LION_PRIV_ORS`: one IntList per OR of `{first clause index, number of arms, leaves in each
arm}` (shape marker bumped to 4, `LION_PRIV_NMEMBERS` 9). Flattening is what makes everything else
free: `lion_collect_targets()` matches an index for each leaf per partition, a Param in a leaf lands
in `custom_exprs` where `set_customscan_references()` and `SS_finalize_plan()` can see it, and the
cost model prices each leaf's bucket page and chain. What the executor derives from the structure is
`inor[]` (a clause is a leaf of an OR) and one *item* per source: a plain clause, or a whole OR.

**Leaves constrain no column of the result.** The other arms select rows this arm says nothing
about, so an OR leaf enters none of the per-column bookkeeping of §10/§14: not `posattnos` (so
`a = 1 OR a = 2` is legal, and the one-positive-clause-per-column rule does not compare an OR leaf
with a top-level clause), not `eqattnos` (so the pinned-column echo never prints a key from an OR
leaf's entry), and not `nonnullattnos`/`nullattnos` (so `count(n) WHERE n = 2 OR a = 17` is NOT
answerable and the query is declined - it would otherwise come out as `count(*)`).

**Cost.** Each leaf is charged its own bucket page and container chain, as if it had been a separate
clause, plus the §15 union term for the merge they take part in: `members × log2(nleaves)` at
cpu_operator_cost, where members is the sum of the leaves' own selectivities times the relation's
rows. `lion_cost_count_rel()` therefore takes the whole clause list and skips the negated ones
itself, rather than being handed a filtered list, because the OR structure names its leaves by
position.

**Safety.** For a union a dead TID may be contributed by a single leaf, so EVERY leaf under an OR
must be read the pinned way until the merged container has been through the visibility-map check
(`lion_source_pinned()`: OR → every child, which is what decides whether the source carries the §9
interlock at all). Leaves under an OR are therefore never materialized: `LionCountSource` gains
`nomaterialize`, which `lion_locate_or()` sets and which is rule 3 of the materialization decision in
`lion_count_sources_cached()`. The §9 argument would in fact still hold for a stale copy under a
union - the intersection is only counted from the visibility map while some positive source is
pinned, and a TID that survives into the counted container is in that source's set too, so its
index's ambulkdelete is blocked - but keeping "every leaf of a union holds a pin" true by
construction means the interlock can be checked by reading `lion_source_pinned()` alone. §15's IN
lists are unaffected and may still be materialized.

**Executor.** `lion_locate_leaf()` is one clause's whole run-time meaning: it locates that clause's
posting sets and returns the tree over them, its leaves numbered from 0, or NULL when the clause
selects nothing (a NULL Param, an empty IN list). `lion_locate_or()` calls it per leaf, concatenates
the sets into one array for the source and renumbers each leaf's tree onto its slice
(`lion_shift_keynos()`), then builds AND nodes for the arms and one OR node over them. An arm with a
leaf that selects nothing is dropped - which is how **a NULL Param arm selects nothing while the OR
of the rest remains** - and an OR with no arm left sets `wheremissing`. A leaf whose key simply has
no entry keeps its set and its leaf node; `lion_sets_satisfiable()` then answers for the whole tree.
Pins: the OR's sets are released by `lion_release_where()` with every other source's, on ReScan, at
End, and at the end of each partition's turn.

**EXPLAIN** prints the arms after the indexes of all the leaves:
`Lion Indexes: ix_c200, ix_c20 ((c200 = 17) OR (c20 = 3))`, an AND arm as
`((a = 1) AND (b = 2))`, and - on a partitioned scan, where there is one index per partition - the
expression alone. A Param is deparsed as `$1` like any other clause value.

**Measured** (2026-09-21, 1M rows of `(c200, c20)`, 18,182 heap pages all-visible, assert build):
`count(*) WHERE c200 = 17 OR c20 = 3` returns 55,000 rows in **0.55-0.62 ms** against the BitmapOr +
Bitmap Heap Scan's **18.3-30.8 ms**, reading 570 containers, skipping all 18,182 heap blocks via the
visibility map and rechecking none. A three-arm form is 0.75 ms against 21.4 ms.
`test/sql/pushdown.sql` covers two- and three-arm ORs, an AND arm, an OR beside a plain clause and
under a GROUP BY, two ORs in one query, IN and IS NULL and multi-key arms, arms whose value has no
entry, Params under `force_generic_plan` (including NULL ones), an exec Param under a LATERAL nested
loop, a partitioned table, a dirty heap and an all-visible one, and the negative cases above - each
compared against the same query with the pushdown off, as a multiset both ways round.

## 20. GROUP BY two indexed columns (v1, implemented)

`SELECT a, b, count(*) FROM t [WHERE ...] GROUP BY a, b` is a nested loop over the two indexes'
entries. Everything §10 says about ONE grouping column is said again per column: its own scalar lion
index, its own collation, its own grouping equality (`SortGroupClause.eqop` = strategy 1 of that
index's opfamily), its own value-representation gate (`lion_index_can_emit_value()` when the key is
printed), and - for a partitioned parent - all of that per partition. Three or more columns are
declined; so is the same column twice.

**Which one drives.** The column with FEWER estimated distinct values is the OUTER one: its entry
scan drives the node exactly as a single group column's does, and the other index is the INNER one.
The choice is made at plan time from `estimate_num_groups()` per column, and the two are swapped
before anything else looks at them, so `groupattno`/`groupidxoid` are always the outer pair and
EXPLAIN names them first. The choice does not change the total intersection work (below); what it
decides is how many entry scans there are and whose keys are held in memory.

**The loop** (`lion_next_group2()`). For each outer entry: hold its key and its posting set - and
that set's pin - in `outercxt` for the whole of its inner loop; for each inner key, locate the inner
posting set, count the AND of (outer group, WHERE sources, inner group) through
`lion_count_sources_cached()`, release the inner set, and emit (outer key, inner key, count) when
the count is above zero. At most two group pins exist at a time, however many distinct values either
column has, which is the pin budget of §9. NULL groups are each column's reserved NULL entry (§14)
and come out like any other; `count(col)` of a group column is 0 in THAT column's NULL group, which
is why there are two target-list kinds for it.

**The inner keys, and why only the keys** (`lion_load_inner_keys()`). The inner index's entries are
read ONCE per relation and only their KEYS are kept, in `innercxt`. An entry's head block or INLINE
payload would be worthless without the buffer pin that goes with it (§9), and holding one pin per
distinct inner value for the length of the scan is exactly what the pin budget forbids - so each
pair re-locates its inner posting set with `lion_posting_set_lookup()`, one bucket page, and the
bucket count is sized to the index's entry count, so that lookup is a shared-buffer hit for every
index small enough for the cost model to have chosen this plan. The alternative - walking the inner
index's entry scan once per OUTER group - needs no memory but re-reads every bucket page of the
inner index `outer_entries` times; it is kept as the FALLBACK for when the keys do not fit the
`work_mem` budget (checked every 256 keys with `MemoryContextMemAllocated()`), because the §16
lesson is that a plan-time bound is only as good as `estimate_num_groups`. `innerkey == NULL` selects
that path, and then the inner key lives in the per-pair context like a single-column group's does.

**Cost** (`lion_cost_count_rel()`). Three terms on top of §10's:

- one `cpu_tuple_cost` per (outer, inner) PAIR, for the inner lookup and the per-pair bookkeeping;
- the INTERSECTION, which is what decides. ANDing two containers costs about the members of the
  smaller of them, so one pair costs about `Min(rows/outer_entries, rows/inner_entries)` member
  steps and the sum over all `outer_entries × inner_entries` pairs is exactly
  **`Min(outer_entries, inner_entries) × rows`** - independent of which column drives;
- and both sides' container bookkeeping at every pair,
  `pairs × (containers(outer group) + containers(inner group))`, which is what makes a pair of
  widely SPREAD groups expensive even when their intersection is empty: two groups whose rows are
  scattered over the whole heap have a container at nearly every container key and the merge steps
  through all of them.

A WHERE source under either grouping is priced as §22 prices a probed source, which for a grouping
means one read of it and nothing per pair: the pairs probe it far more often than it has pages, and
the copy §9 makes of it on its second use is probed in memory. So none of the three terms above
moved when §22 recalibrated the pages, and neither did any cardinality this section accepts or
refuses.

Measured (2026-09-21, 200k rows of a 100-byte-wide table, uncorrelated columns, assert build), node
against the sequential aggregate: **20 × 2 groups 5.6 ms against 37.3 ms**, **200 × 2 14.9 against
39.3**, **200 × 20 60.7 against 36.8**, **20000 × 2 169.8 against 51.6**, and 20000 × 200 far worse.
The model chooses the first two and refuses the rest, which is where the measurements say the
crossover is. Note the deviation from this section's sketch, which expected 200 × 20 to win: on
UNCORRELATED data it does not, because the member term above is 20 × 200,000 = 4M container-member
steps against 200,000 heap tuples for the sequential scan. Clustered data would change that - a
posting set confined to a few container keys makes most pairs' merges terminate at once - and the
model does not model correlation, so it refuses those too. That is conservative, and it is the open
item this section leaves.

**Partitions** work exactly as §16's single-column case: one partial aggregate per (outer, inner)
pair per partition, streamed as each partition is counted, with core's Finalize HashAggregate on top
combining them; both grouping columns must be hashable. `LION_PRIV_PARTS` carries the inner index Oid
per partition next to the outer one. Note that a partitioned plan leaves BOTH index Oids in
`LION_PRIV_OIDS` invalid, so nothing in the executor may decide "is there a second group column?"
from an Oid - `groupattno2 != 0` is the test, and getting that wrong was a real bug: the inner
group's source was left out of the count and every pair came back with the outer group's count.

**EXPLAIN** prints both indexes and both columns: `Lion Indexes: ix_b (b), ix_a (a)` and
`Group Key: b, a`, in outer-then-inner order. `test/sql/pushdown.sql` covers the results against the
ordinary plan (as a multiset both ways round) with and without a WHERE clause, with NULLs in either
column, with `count(col)` of either, under an OR restriction, on a dirty heap and a clean one, on a
partitioned table, and pins the plan choice for 20 × 2, 200 × 2, 200 × 20, 20000 × 200 and
20000 × 2 with nothing disabled.

## 21. Sorted key directory (format version 4, implemented)

Replaces the hash-bucket entry directory of §4 with a B-tree of entries keyed by the index key.

Why. The hash directory had three measured costs: an index created empty kept its 64 buckets
forever (100k unique keys: 0.104 ms vs 0.321 ms per 100-key IN after growth), a large IN list was one
random bucket page per value, and entry iteration (GROUP BY, IS NOT NULL, verify) was in hash order,
so ordered output and range-bounded entry walks were impossible. A B-tree directory grows by
splitting, makes a sorted IN list a near-sequential leaf walk, and gives GROUP BY output in key order
(the Sort above the node disappears when the query's ORDER BY is the group key).

Structure. Meta page (block 0) points at a `root` and records the root's `height` and the number of
`dirpages` the directory has; `nbuckets` is gone (the field is kept at zero for layout stability).
Directory pages carry LION_PAGE_DIR (internal) or LION_PAGE_BUCKET (leaf; the flag name is kept so
verify/stats/page-inspection tools keep working), plus LION_PAGE_ROOT on the current root and
LION_PAGE_INCOMPLETE_SPLIT on a page whose split has not finished. The special area gains `level`
and a `leftlink` (24 → 32 bytes after MAXALIGN, which is why every item on every page moves and the
version has to be bumped). Leaf pages hold LionEntryTuple items in key order, exactly as bucket
pages did; internal pages hold downlink pivots, LionEntryTuple-shaped with LION_ENTRY_DOWNLINK and
`head` = the child block, so the key helpers apply unchanged. Every non-rightmost page's FIRST item
is its high key (LION_ENTRY_HIGHKEY, key only), and the leftmost page of every level begins with a
LION_ENTRY_MINUSINF downlink that compares below everything. Reserved NULL and EMPTY entries sort
first (NULL < EMPTY < value) via a kind prefix in the comparison, not in the stored key, so they
live on the leftmost leaf and are reached by the ordinary descent. Multi-key classes: the key type
is the STORAGE type (§17), which is what is compared.

**An entry tuple is capped at LION_MAX_ENTRY_SIZE**, which is the largest item a page can hold minus
the largest pivot (`MAXALIGN(LION_ENTRY_HDRSZ + LION_MAX_KEY_SIZE) + sizeof(ItemIdData)`), because a
leaf that holds ONE entry must still have room for the high key a split would give it. A payload
that would exceed it spills onto container pages exactly as one that exceeds `inline_limit` does.
*(Not in the first draft of this section, which did not notice that a 2000-byte key with a
4096-byte inline payload plus a 2000-byte high key is 40 bytes over a page.)*
*(Nor did the first build: ambuild spilled at `inline_limit` alone, so an INSERTer could leave a
posting set that INSERT had spilled but REINDEX, VACUUM FULL or a restore kept INLINE and could not
write - 3900 rows of one 1990-byte key made a 6092-byte entry. Build and INSERT now take the bound
from one function, lion_inline_max(); the 2026-09-23 review found it.)*

### The order

    KEY COLUMN (§24), then kind (MINF < NULL < EMPTY < VALUE), then proc 4 under the column's
    collation, then the hash, then a bytewise comparison of the stored datum

The leading key-column term is §24's; a single-column index has one value for it and everything
below reads as it was written. It comes FIRST so that each column's entries are one contiguous run:
a lookup is an (attno, key) descent, an entry scan bounded to one column is the leaf walk from its
first entry to the first entry of the next column, and an item of another column is settled by that
one comparison without calling a single opclass function - which is what lets one descent cross the
entries of columns whose key types it knows nothing about. The minus-infinity downlink carries
column 0 and therefore still sorts below everything.

The kind and what follows it are the **prefix** within one column. Two entries whose prefixes tie are candidates for being the same
key, and only the opclass EQUALITY decides: a descent lands on the first item of the prefix run and
scans it - across right links if it spans pages - applying strategy 1. That is what makes citext's
`'Alice'` and `'alice'` ONE entry: `citext_cmp` returns 0 and `citext_hash` agrees, so they tie, and
`citext`'s `=` says they are the same key. The bytewise tail exists so that the order is TOTAL even
for a type with no btree opclass at all, where the prefix is only (kind, hash); it is never what
decides that two entries are distinct, because entries in one prefix run are found by equality.

*(Deviation from the first draft, which said the order was simply "kind, then cmpproc, then hash,
then memcmp". Stopping at the bytewise tail would put citext's two spellings at two different
positions and therefore in two entries - the very thing the section requires a test for. The run
scan is what reconciles the two, and it is nbtree's own handling of duplicate keys.)*

A search key whose stored form is not available - every lookup, which has a Datum and not the bytes,
and every cross-type lookup, which cannot produce them at all - compares as the SMALLEST member of
its own run, which is exactly what positions it at the run's start. The INSERT path does have the
bytes (it has just built the entry tuple), and uses them to place the new key at its exact position,
which is what keeps the on-disk order total.

Every rule below is **per key column** (§24): each column has its own opclass, so its own key type,
hash, equality, comparison, collation, multi-key extraction and its own answer to "ordered?".
`LionState` is that per-column state and `LionIndexState` holds the meta page and the array of them;
`lion_get_state()` hands back column 1, which is all a single-column index has.

**The ordering source.** Every default opclass gains support proc 4, the btree comparison of the KEY
type: the type's default btree opclass's support function 1, named in the SQL script
(`bttextcmp`, `numeric_cmp`, ...; `enum_cmp` for anyenum, `citext_cmp` for citext, `bttextcmp` for
`tsvector_ops`, whose STORAGE type is text). `array_ops` has none - its STORAGE type is the
polymorphic `anyelement` - and falls back to the element type's default btree comparison through
`lookup_type_cache(TYPECACHE_CMP_PROC_FINFO)` at `lion_fill_state()`, exactly as its hash does, and
under rule 2 below: its keys are compared with the element type's own default equality, which is the
one that borrowed comparison agrees with.
`lionvalidate()` accepts proc 4 as OPTIONAL, with signature (keytype, keytype) → int4, and is the one
support function allowed to be cross-type.

A type with no btree opclass at all - `xid`, `cid` - has no ordering: `LionState.ordered` is false,
the directory is ordered by (kind, hash, bytes), which is a complete order but not the type's,
`lion_index_stats()` reports `ordered = false` and the count pushdown claims no pathkeys.

**Three rules decide what the directory is ordered by** (`lion_fill_state()`), and all three say the
same thing: the order the BUILD lays entries out in and the order a SEARCH descends must be one and
the same, and both must agree with the opclass EQUALITY, because the directory holds exactly one
entry per equality class.

1. An opclass WITH proc 4 is ordered by it.
2. An opclass WITHOUT proc 4 may BORROW the key type's default btree comparison only when its own
   equality operator IS the key type's default btree equality (compare the strategy-1 operator OID
   against `get_opfamily_member()` of the type's default btree opfamily). Otherwise it is
   UNORDERED. *(Deviation from the first draft, which borrowed unconditionally. The borrowed
   comparison can be FINER than the opclass's equality, and then the two members of one equality
   class sort to two different positions while the directory keeps only one entry: the
   case-insensitive `lion_lower_ops` of `test/sql/pushdown.sql` stores `'A'` and `'a'` in one entry,
   `bttextcmp` separates them, and a lookup for the spelling the entry was NOT created with
   descended past it and found nothing. An unordered directory ties wherever the hash ties, which is
   exactly where the run scan applies the opclass equality, so the same class works.)*
3. Either way the ordering needs a `<` OPERATOR THAT SORTS WITH THE SAME FUNCTION, because ambuild's
   tuplesort is driven by an operator. The key type's default `<` qualifies only when the comparison
   is the key type's default one; a comparison of the opclass's own is looked for in the btree
   opfamily that uses it as its `BTORDER_PROC` (a `pg_amproc` scan, once per relcache build, and
   only for an opclass that names a comparison no built-in one does). The candidate is verified with
   `get_ordering_op_properties()`, which is the same catalogue path
   `PrepareSortSupportFromOrderingOp()` takes, so "the sort will use this function" is not a guess.
   An ordering the build cannot reproduce is NO ordering: the index is unordered instead, which is
   still correct. *(Deviation from the first draft, which took the type's default `<`
   (`TYPECACHE_LT_OPR`) whatever the comparison was, so an opclass with a comparison of its own
   sorted one way at build time and searched the other: every key ended up where no search looked
   for it, and verify() said so.)*

**The ordering must agree with strategy 1**: two keys the comparison calls equal must be equal to the
opclass. The directory holds one entry per equality class and finds it by scanning the run the
comparison ties, so a comparison FINER than the equality (one that separates two equal keys) would
create two entries for one class. Rule 2 above is what keeps a BORROWED comparison inside that
promise; a proc 4 an opclass names itself is still the author's responsibility, and the AM cannot
detect a bad one.

**The order is the index's, not the catalog's** (2026-09-24 review of §28). The three rules above
decide how a BUILD lays the directory out; they used to be applied again at every relcache build,
from the catalog as it stands, and nothing stored the answer with the index. The catalog can change
under an existing index: a btree opclass created later whose support function 1 is a lion
opclass's proc 4 makes that proc 4 sortable (rule 3), so a directory built in HASH order was read
in value order - `a BETWEEN 100 AND 2000` counted 10 rows for 19,010, `a = 1234` 0 for 10, and an
insert put its entry where no later search looked - and dropping that btree opclass does the
reverse. A proc 4 added to a family by `ALTER OPERATOR FAMILY ... ADD FUNCTION` is a loose member
that can be dropped and replaced by another function, and a borrowed comparison (rule 2) follows
the key type's default btree class, which can change too.

So the build RECORDS the order on the meta page, in three words of what was reserved space
(`order_flags`, `ordered_cols`, `order_ident`; format version unchanged, as for §25's `wal_mode`):
which key columns are ordered, and a hash of the comparison each ordered one was built with -
of what the function RUNS (`lion_proc_ident()`): `prosrc`, which is the C symbol of an internal or C
function and the body of a SQL or PL one, and `probin`, the library of a C function. Neither an Oid
nor a name would do. pg_upgrade keeps an index's files but recreates its user functions under new
Oids; a schema-qualified name made `ALTER EXTENSION citext SET SCHEMA` - citext is relocatable, and
its type and `citext_cmp` move with it - brick every citext lion index until REINDEX (the first
version of this fix did exactly that, the coordinator's review caught it); an unqualified one would
do the same to `ALTER FUNCTION ... RENAME`. The source survives all three and still catches the
common ways of changing a comparison: a loose proc 4 swapped for another function, a string body
replaced by `CREATE OR REPLACE`, a borrowed comparison now taken from another default btree class.
The argument types are left out on purpose: they are the key type, which the index fixes, and a
moved or renamed type is the same type. A RENAME of a user opclass's proc 4 therefore changes
nothing, and replacing its body is an ERROR until REINDEX, which is the intended pair.

**It is a best-effort guard, not a guarantee** (the 2026-09-24 external review of 118f623 found two
gaps, both fixed below, and the limits are stated here). A function's source does not include what
it CALLS: a string body that calls another user function, and that function replaced later,
compares differently with an unchanged source; so does a C function whose shared library is
swapped under the same symbol. Nothing short of knowing what a function does could catch that, and
core's btree has exactly the same exposure - PostgreSQL's rule is that changing the behaviour of an
operator class's function requires a REINDEX of every index that uses it - so that is this index's
rule too. The guard catches the common, direct changes; it does not certify the rest.

- **SQL-standard bodies are never ordered by.** A `RETURN` function keeps its parsed body in
  `prosqlbody` with an empty `prosrc`, so replacing an ascending body with a descending one left
  the identity unchanged and an existing index answered 0 for 10. The tree cannot be hashed as it
  stands - it embeds Oids that pg_upgrade does not keep for user objects - and neither can its
  deparse, which depends on the session's `search_path`, so two sessions would disagree. A
  normalised hash that replaced every user-object Oid by a stable identity would work and is far
  more code than the case deserves. So a BUILD whose comparison has a SQL-standard body lays that
  column out in HASH order, with a NOTICE naming the column and the fix (a string body or a C
  function). That is chosen over an ERROR because a hash-ordered column is still a correct and
  fully usable index - equality descends by hash, ranges take §28's test-every-entry walk, only the
  count pushdown's pathkeys are lost - and no later change of the comparison can make a hash
  order wrong; refusing the CREATE INDEX would only take the index away. A body that BECOMES
  SQL-standard later is seen, since `prosrc` changes to the empty string. The one flip left is an
  index built before the order was recorded (`order_flags` = 0) with a SQL-standard comparison: it
  is read from the catalog, which now says hash order; REINDEX it.
- **A backend that already has the index open checks too.** The state is cached in the index's
  relcache entry (`rd_amcache`), and `CREATE OR REPLACE FUNCTION` invalidates pg_proc, not the
  index, so an open backend went on reading an ascending directory with a descending comparison
  (0 for 10, even with `prosrc` changed). A syscache callback on PROCOID, registered once per
  backend, now only counts pg_proc invalidations - it runs while invalidations are processed, where
  no catalog may be read - and `lion_get_index_state()` checks the cached state's OWN comparisons
  (the functions its FmgrInfos call) against the recorded identity whenever the count has moved
  since it last passed; a state that passes is good until the next change. A comparison the catalog
  would resolve differently today but this state does not call cannot hurt it, and the next state
  built is checked in full by `lion_fill_index_state()`. The check is skipped while the caller holds
  a buffer lock or is in a critical section (`InterruptHoldoffCount`, `CritSectionCount`): it reads
  the catalog, which is no business of a page change half-way through, and the next call made with
  nothing held does it. The check therefore runs at the
  next use of the index in that backend after the change is seen. A query already running when
  another session commits the change is not protected: its plan and its FmgrInfos were set up
  before, and a SQL function's cached plan follows the new body at its next call - the same
  window a btree scan has, closed by the same rule. `test/isolation/order_ident_cache.spec` counts
  in a backend, replaces the comparison from another (a string body, and a SQL-standard one), and
  requires the REINDEX error in the first backend and the right answer once the body is restored.

`lion_fill_column_state()` still resolves the comparison from the catalog, but the
RECORDED bit decides whether the column is ordered: a directory built in hash order stays in hash
order whatever btree opclass appears later, and one built in value order is read in value order
without the sort operator, which only the build's tuplesort ever needed. If an ordered column's
comparison no longer resolves at all, or resolves to a function with a different source than the
recorded one, opening the index is an ERROR with a REINDEX hint rather than a quietly different
order (within the limits of the guard, above). Readers and
writers use the same stored order, so backends whose relcache entries were built at different
moments cannot disagree any more. REINDEX asks the catalog again. `ambuildempty()` records the
order of the empty directory the same way. The meta page reaches WAL as it always has - the build's
bulk write and `ambuildempty()` log it as a full page image in either WAL mode, and a later split's
meta update carries the whole struct - so replay needs nothing new. `lion_index_verify()` already
checks every leaf and every pivot against the order the index is READ in, which is now the recorded
one. An index built before the record existed has `order_flags` = 0 and is read as it was, from the
catalog; REINDEX records its order. `test/sql/ordering.sql` is the review's repro both ways round
(hash order kept after the btree opclass appears, value order kept after it goes), with inserts in
between and verify, a swapped loose proc 4 refused, a renamed proc 4 accepted and a re-bodied one
refused, a SQL-standard comparison built hash-ordered (NOTICE) and still exact after its body is
reversed, and a citext index that keeps answering exactly (equality, ranges, inserts, verify)
after `ALTER EXTENSION citext SET SCHEMA` - moved back afterwards for the tests that follow.

**A relcache flush must not free the state, and must not make the write path read the meta page**
(2026-09-24, found under `debug_discard_caches`, which flushes on every catalog read; ordinary
invalidation traffic can do the same at any lock acquisition, only rarely). Two things went wrong.
A flush of an index's relcache entry `pfree()`s `rd_amcache`, and `rd_amcache` WAS the
`LionIndexState`: every caller that took the state and then read the catalog before it was done -
the order check itself, a scan resolving a cross-type probe, `lion_index_verify()` comparing text
keys under a collation - held a pointer into freed memory; the order check read a garbage identity
and refused every index, and verify crashed. `rd_amcache` is now a HANDLE (`LionAmCache`) on a
state allocated beside it in `rd_indexcxt`, so a flush frees only the handle and a pointer taken
earlier stays valid - no longer the entry's current state, and the next `lion_get_index_state()`
builds a new one, as it always did after a flush. That holds because a flush of an OPEN index entry
with its support info loaded is, on every release from 16 to 20, the in-place reload
(`RelationReloadIndexInfo()`: the index branch of 16's and 17's `RelationClearRelation()`, 18-20's
`RelationRebuildRelation()`), which frees `rd_amcache` and keeps `rd_indexcxt`; the context goes
only with the entry, in `RelationDestroyRelation()`, which asserts a reference count of zero. So the rule is that a state is valid
while the caller holds the index open - every caller does, and none keeps one across
`index_close()` - and a stale state is a correct one: its content is what was built, only no
longer the entry's. A flush therefore leaves one state behind in `rd_indexcxt` per flush of an open
index until the entry is destroyed, as it already did for the column states. And `lion_wal_mode()`, asked for when a record
begins - in a split, with the meta page held EXCLUSIVE - rebuilt the state after a flush, which
reads the meta page: a second lock on a buffer the backend already holds, an assertion failure on
a cassert build and a wait for ever on a production one. The mode never changes for a
relfilenode, so it is remembered per relfilenode the first time a meta page is read
(`lion_index_meta_wal_mode()`), and the write path never reads one. `test/sql/discard_caches.sql`
runs counts, ranges, bitmap and plain index scans, a GROUP BY, a count(DISTINCT), an insert and
verify on text, int, citext and a custom ordered opclass under `debug_discard_caches = 1` where the
build allows it (assert builds) and without it elsewhere, with one expected output.

**The opclass's functions run under a leaf's share lock.** A descent's binary search, the run scan
and the range walk of §28 call proc 4, the equality, and - on an unordered column - the range
operator itself while holding a directory leaf SHARE-locked. A SQL-language function that reads
the same index would ask for that leaf's lock again from the same backend, and buffer LWLocks have
no deadlock detection: an EXCLUSIVE request queues behind our own share lock and the backend hangs.
Core's own btree has the same exposure with its support functions. Only a superuser can create an
operator class, so this is a property of the opclasses one installs, not an attack surface; the
shipped classes call C functions that read no relation.

**Cross-type searches** (`int4col = 123::int8`) need an ordering of a STORED key against a value of
another type, and it is resolved ONCE for the single-value lookup, the batched one AND the bitmap
scan (`lion_probe_init()` / `lion_probe_find()` in lion_count.c, declared in lion_count.h and used by
lion_scan.c), because all three walk the same tree and a path that descended where another scans
would read the directory in an order it is not in. The outcomes, in this order:

- an UNORDERED column (rule 3 below can make one even when its family names comparisons): the hash
  and the cross-type equality only. No comparison of any kind is used on a directory in
  (kind, hash, bytes) order - the family's cross-type proc 4 included - and the family's
  cross-type hash agrees with the stored keys' by the family's contract, so the descent is exact;
- the family's cross-type proc 4 (`btint48cmp` and friends; `integer_ops` and `float_ops`, the only
  families with cross-type equality, carry them): descend as usual;
- otherwise a BINARY coercion of the value to the key type (`find_coercion_pathway()` returning
  `COERCION_PATH_RELABELTYPE`: the same bytes, varchar to text): the value becomes one of the
  index's own and hash, equality and ordering are all the index's own, which is consistent by
  construction;
- otherwise the leaves are walked with the cross-type EQUALITY (`lion_dir_find_by_scan()`), which is
  correct and linear.

*(Deviation from the first draft, which described only the walk and let
`lion_posting_set_lookup_many()` call the descent directly: an `IN` list of `int8` values against an
`int4` index whose family had no cross-type proc 4 descended a value-ordered tree comparing hashes
and returned nothing at all. `test/sql/directory.sql` §12 builds such a family.)*

*(Three further deviations, from the 2026-09-22 review. (1) The coercion step used to take any
IMPLICIT cast function too. Implicit is not lossless: `text` → `name` truncates to 63 bytes, so for
a family with `name = text` equality and no cross-type ordering a long text that is no stored name
became one and was counted. PostgreSQL cannot say that a cast is a bijection, so only a binary
coercion is taken; §16 of `directory.sql`. (2) The bitmap scan resolved its own comparison and used
the family's cross-type proc 4 without asking whether the column was ordered, binary-searching a
hash-ordered directory in value order; it now goes through the shared resolution; §15. (3) See
Readers, below, for how a LIST of cross-type values is sorted.)*

### Operations

- **Lookup**: descend from the root with a binary search per page, SHARE locks, **releasing the
  parent BEFORE locking the child**. *(Deviation from the first draft, which said "lock coupling
  parent→child as nbtree does". nbtree does not couple downwards - `_bt_relandgetbuf()` unlocks the
  page it came from first - and the reason is not thrift: a split holds the CHILD while it locks the
  parent, so coupling downwards would close a deadlock cycle that buffer locks have no detector for.
  What makes the un-coupled descent safe is the right links: a searcher that lands on a page whose
  high key no longer exceeds its key moves right, which is where the split put the items.)* The leaf
  is taken in the caller's mode directly (the descent knows it is at level 1), so no lock is ever
  upgraded. The leaf pin remains the §9 pin for INLINE sets.
- **The root** is cached in `LionState` and validated by the LION_PAGE_ROOT flag, which a root split
  clears on the page it demotes - nbtree's BTP_ROOT trick. A lookup therefore costs no meta-page
  visit at all until the root really moves.
- **Insert of a new entry**: descend with the leaf EXCLUSIVE; scan the prefix run for the key; if it
  is absent, walk back to its exact bytewise position on that leaf and insert. On no room, split.
  **Find-or-create is one serialised operation**: the directory holds one entry per equality class,
  so no second writer of the same key may run its lookup between this writer's unsuccessful lookup
  and its insert. On one leaf that is automatic - the leaf is held from the lookup to the insert. A
  prefix run that SPANS pages (which needs an opclass whose comparison ties for distinct entries: an
  unordered class whose hash collides, as `test/sql/directory.sql` §11 and
  `test/isolation/dir_insert_race.spec` build) is handled by the **guard**: the leaf the run is
  entered from - the leaf a descent for the run's prefix lands on, which the lookup started on - is
  NOT released when the scan steps right; the scan couples its steps (next page locked before the
  current one is released) and, if the key is absent, gives up the leaf it ended on and hands the
  guard back to the insert, which walks right from it under the same coupling to the key's exact
  bytewise position, finishing any split it steps into, and inserts there. Every INSERT of a key of
  that prefix - one that creates it or one that adds to an existing entry of the run - descends to
  the guard first, so while it is held nobody else can look the key up, and the insert needs no
  second existence check. (VACUUM re-finds entries with an exact key and may land further right,
  but it only ever rewrites or deletes an entry, never creates one.) The guard is a fixed point while it is held: it cannot split (it is locked), a
  page to its left can only split into pages whose high keys are still below the prefix, and a
  descent always ends on the first leaf whose high key's prefix is at or above the search key's.
  *(Deviation from the first draft, which RELEASED the leaf and re-descended with the exact key and
  inserted without looking again: two writers of one new key could both miss and both insert, and
  the rows under the entry no lookup returned were lost to counts and scans. verify() now reports
  two entries of one equality class, byte-identical or not, as corruption.)*
  **Lock order.** The guard and the page the walk is on are both leaves, taken left to right - the
  order every directory writer already uses (`lion_dir_place_again()`, a split's old right
  sibling) - and nothing ever waits for a leaf to its LEFT while holding one; a descent holds
  nothing, an ascent (a split's parent insertion, a repair) holds leaves and waits only for
  internal pages and the meta page, and nobody holding those waits for a leaf. So a writer holding
  the guard and splitting the target page further right is nbtree's "hold the child, lock the
  parent" with one more leaf held on the left, and it cannot close a cycle. Container pages still
  come after directory pages, and the guard is released before any of them is touched on the
  update path. Injection point `lion-dir-add-entry-spanning` fires with the guard held, between the
  unsuccessful lookup and the insert.
- **Split**: allocate the right sibling (`lion_alloc_buffer` with reuse = true is fine; directory
  pages are never posting-set roots), rebuild both halves from a private copy of the page with the new
  item inserted, cut at half the bytes - or, when the page is rightmost and the item goes at the very end,
  put only the new item on the right and walk the cut back only as far as the high key the left page
  now needs, so an ascending key sequence fills its leaves completely (without that walk-back the
  cut is rejected outright, because the page being split is full, and an ascending build lands at
  50%: measured 12.2 MB against 6.2 MB for 100k ascending bigint keys) - set
  the high keys and the sibling links, flag the left page INCOMPLETE_SPLIT, and write ONE generic
  record for (left, right, old right sibling, meta page) = 4 buffers. Then insert the downlink into
  the parent, and clear the flag in a THIRD record.
  *(Deviation from the first draft, which had two records with the flag cleared "in a second record"
  together with the downlink, as nbtree does. GenericXLog takes at most four buffers and a parent
  SPLIT already needs four of its own, so the child cannot join that record. Three records are safe
  because the repair is idempotent: it looks for the downlink before inserting one. The left page is
  held EXCLUSIVE from the first record to the last, which is the property nbtree's README says must
  hold - two writers that both saw the flag would both insert the downlink.)*
  The meta page is in the record because it counts the directory's pages, which is what lets the cost
  model tell a directory page from a container page without reading the index.
  **The two walk-backs can pull in OPPOSITE directions and leave no cut at all**, and that is no
  longer an error (2026-09-22 review of this section). An 8 KiB page has room for one item plus one
  pivot (6092 + 2036 = 8128) but not for two items plus a pivot, so a page holding two large entries
  under a large high key would have no two-way cut for a third large item between them. The split
  then runs in two steps: the page is split as it STANDS, which always has a cut - k = 1 puts one item
  plus its high key on the left and a suffix of what the page already held plus its old high key on
  the right, and both were on the page a moment ago - and the item is then placed again on whichever
  half now owns it. That terminates, because every split moves at least one item off the page and a
  page holding one item always takes a second. With today's size caps the shape is in fact
  unreachable - a fresh entry is at most 2052 bytes on a page and a replacement grows by at most a
  few dozen - but the caps are three independent constants and none of them should be load-bearing
  for whether an INSERT errors out. **A posting-tree internal page needs none of this** (§22): its
  pivots are fixed size, so the middle cut always fits.
- **Root split** is ATOMIC instead: left, right, the new root and the meta page are four buffers, so
  the meta page never names a root that does not exist and a root is never flagged. The old root
  loses LION_PAGE_ROOT in the same record, which is what makes every other backend's cached root
  detect the change.
- **The repair rule**: any WRITE descent that lands on a page with INCOMPLETE_SPLIT finishes the
  split before touching the page, and so does `lion_dir_find_parent()` when it lands on such a
  parent. Finishing means: take the high key as the separator, find the parent by descending with a
  key the page itself holds (or, for a page VACUUM has emptied, by scanning the level from its
  leftmost page), insert the downlink unless it is already there, and clear the flag. Injection point
  `lion-dir-split-incomplete` fires between the first record and the second;
  `test/recovery/run.sh` phase 1c crashes the server there and proves the next insert repairs it.
- **Entry rewrite in place**: as today, on the leaf, EXCLUSIVE. A grown entry that no longer fits
  splits the leaf and is placed by the split, instead of spilling onto a container page as it had to
  with the hash directory.
- **Delete** (VACUUM, §18): entries are deleted with `PageIndexMultiDelete`, which compacts. *(The
  `PageIndexTupleDeleteNoCompact` of §18 existed to keep entry offsets stable for readers parked on
  a page; a sorted directory cannot offer that anyway - an insert in the middle of a leaf shifts
  every offset after it - so the readers changed instead, below, and the compaction reclaims the line
  pointers.)* An empty leaf is NOT unlinked in this version (leaf deletion needs nbtree's half-dead
  protocol; documented limitation, and the pages are few).
- **Ordered iteration**: leftmost leaf, then right links, SHARE lock one page at a time, pin held
  while an INLINE set from that page is being counted exactly as the bucket walk did.

### Offsets are not names any more

The hash directory only ever APPENDED to a bucket page, so `(page, offset)` named an entry for its
whole life, and two readers relied on it: `lion_entry_scan_next()`, which gives up its lock between
two entries, and VACUUM's pass 2, which refers to the entries pass 1 found while the leaf is
unlocked. A sorted directory inserts in the MIDDLE of a leaf, which shifts everything after it, and
splits it, which moves the upper half away. Both readers therefore changed:

- **The entry scan resumes at a KEY**: "the first key above the last one I returned". Nothing is
  skipped or returned twice, because splits move entries only rightwards onto a page the walk has not
  passed and their keys are still above the last one returned; a deleted entry is simply gone, and
  only an EMPTY posting set is ever deleted, so its group had nothing the scan's snapshot could have
  counted. An entry INSERTED behind the walk is missed, which is the same freedom the bucket walk
  had. `test/isolation/dir_split_scan.spec` parks a GROUP BY between two entries and splits the leaf
  under it.
- **VACUUM carries the entry's KIND and KEY** next to the offset, and re-validates before every use:
  one entry per key, so equal stored bytes mean the same entry. A hit at the remembered offset is the
  common case, a scan of the page catches an insert that shifted it, and anything else means the
  entry moved to another leaf and is found again by a descent - **with nothing else held**, because
  taking a directory lock while holding a container page would close exactly the cycle §11's waiting
  rule exists to avoid. The final delete step validates the same way and simply skips an entry that
  has moved; the next VACUUM gets it.

### Bulk build

The tuplesort's sort keys are `(key ASC NULLS FIRST, [kind], code)` for an ordered opclass and
`(hash, kind, code)` for an unordered one, so the entries come out in directory order. Neither
LEADS with `kind`, although the directory order does, and that is deliberate: a reserved entry has
no key at all, so its NULL sorts before every value with NULLS FIRST, and for an unordered opclass
the reserved entries hash to LION_NULLKEY_HASH = 0 which is the minimum - so both give exactly the
(kind, key, hash) sequence. Leading with `kind` gives the same ORDER and
is much slower: tuplesort compares the leading key from a datum it precomputed and every further
key by fetching the attribute out of the tuple, and `kind` is the same value for every real row, so
every comparison would fall through to a fetch. Measured on one million distinct bigint keys: 1.43 s
of sort with `kind` leading against 0.19 s with the key leading, and 2.04 s against 1.18 s for the
whole build (HEAD's hash-directory build of the same index is 1.40 s). The three fetched columns are
for the same reason the fixed-width ones, first in the tuple descriptor, so that their offsets are
cached.

**`kind` is a sort key whenever the leading one can TIE across kinds**: for a multi-key opclass,
which has an EMPTY entry as well as a NULL one and neither has a key to tell them apart, and for
every UNORDERED opclass, whose leading key is the hash. The grouping pass starts a new entry
whenever the kind changes, so a real key that hashes to 0 interleaving with the reserved NULL rows
- code by code, which is how the sort leaves them - wrote the NULL entry out several times over and
`IS NULL` then answered from whichever of them the descent reached first. An ordered non-multikey
class needs no such key: its only reserved kind is NULL, whose sort key is NULL, and no real key is.
*(Not in the first draft, which added `kind` for a multi-key class only. `test/sql/directory.sql`
§11 gives an `xid` class a deliberately coarse hash so that every value collides with the reserved
entry.)*

**The hash column is int8, zero-extended**: it is a uint32 and the directory compares it as one, and
sorting it as int4 would put everything above 2^31 first - which for an unordered opclass IS the
order, and verify() catches it.

**A hash COLLISION is ordered at flush time.** The sort brings the tuples of one hash together but
says nothing about the several distinct keys inside it, so the grouping pass opens a builder per key
in the order the first TID of each happened to arrive. `lion_flush_builders()` sorts the open
builders with the directory comparator - (kind, proc 4, hash, stored bytes), the same order
`lion_cmp_entry()` applies to two entries on a leaf - before writing any of them out. There is one
builder unless the hash function collides, so the sort costs nothing in the normal case. *(Not in
the first draft, which flushed them in creation order: the leaf items came out unsorted, which
breaks the binary search and lets the key-based resume of `lion_entry_scan_next()` skip entries.
verify()'s "strictly increasing within a page" check is what catches it.)*

Pass 1 of the old build is GONE. It existed only to count distinct keys so that the bucket count
could be sized; the one remaining pass counts them as it groups, so the build reads the sorted data
once instead of twice and the tuplesort no longer needs TUPLESORT_RANDOMACCESS.

Leaves are filled left to right at `fillfactor` (reloption, 10..100, default 90), and the internal
levels are built bottom-up in the same pass, nbtree's `_bt_buildadd` shape: one open page per level,
written out when the item that does not fit arrives - that item's key is the page's high key, and the
page's own first key is the downlink it hands to the level above. The first page of every level gets
the minus-infinity separator. The level whose last page is also its first is the root. An empty table
gets a single leaf that is also the root.

**`fillfactor` is about the LEAVES only.** An internal level is packed to
`LION_NONLEAF_FILLFACTOR` = 70, nbtree's own separate non-leaf fill, and takes at least
`LION_ABS_MIN_DOWNLINKS` = 2 downlinks whatever the fill target says (it aims for 3 and settles for
2 when the keys are too large - three LION_MAX_KEY_SIZE pivots plus a high key do not fit a block).
Without that floor, `fillfactor = 10` and 512-byte keys leave ONE downlink on an internal page,
every level is then as large as the one below it, and `lion_build_finish_dir()` climbs for ever
without reaching a root. It now also checks the invariant it relies on - each level strictly smaller
than the one below - and errors out rather than looping. *(Not in the first draft, which applied the
leaf budget to every level and said no floor was needed.)*

**The high key is accounted for with nbtree's "last item becomes the high key" rule.** A page's high
key is a copy of the FIRST key of the page to its right, which is the item that did not fit; keys
are variable length, so one of LION_MAX_KEY_SIZE arriving behind a page full of short ones needs
room the page does not have, and reserving for the incoming key alone (what the first draft did)
overflowed the page and failed the build outright. `lion_build_level_flush()` therefore moves
trailing items to the next page until the high key fits, and the first of THEM supplies it - a pivot
copy of a key that was already on the page, which is never larger than the item it came from, so the
reserve is exact by construction. `test/sql/directory.sql` §8 puts a 2000-byte key behind short ones
at every distance from a page boundary, at the default fill and at 100.

### Readers

`lion_posting_set_lookup()` is a descent. `lion_posting_set_lookup_many()` sorts the values into the
directory order and locates them in ONE left-to-right leaf walk, stepping right while the next value
is at most `LION_LOOKUP_WALK_MAX` = 8 pages ahead and descending again when it is further - so a
dense list pays for the leaves it crosses and a sparse one for a descent each, instead of either
always. Duplicates are dropped twice over: bytewise between neighbours (free, and it saves the
lookup) and then by comparing the located entries' STORED KEYS with the index's own equality, which
is what §15's disjoint sum needs and is stronger than the `(page, offset)` identity it used to use
(offsets move now).

**The walk only steps right, so the sort must BE the directory order.** Values of the index's own
type (or binary-coerced to it) are sorted with the column's comparison, or by hash for an unordered
column, whose directory leads with the hash. A cross-type list resolved through the family's
cross-type proc 4 cannot be sorted by that function - it compares a stored key with a value, not
two values - so it is sorted with the family's OWN proc 4 for (value type, value type), the
family's statement of how it orders that type; a family without one gets no walk, and every value
descends by itself. The value type's DEFAULT btree order is never used: it is the directory order
only when the opclass happens to sort that way, and against a reverse comparison every value but
the first was looked for to the right of where it lives (`directory.sql` §14). A list whose
resolution came out as "walk the leaves" takes that fallback per value, as the single lookup does.
*(Deviation from the first draft, which sorted every cross-type list with the value type's default
comparison.)*

**A walk that followed a prefix run across a page boundary descends again for the next value.** It
then stands to the right of where the run begins, and the next value may be a hash collision of the
same run stored on a page it has passed; otherwise the leaf a lookup lands on is the one its run
begins on, so a value sorted after it is there or further right. *(Not in the first draft: a list
over a coarse-hash unordered class lost every value stored left of where the previous one was found,
`directory.sql` §17.)* `lion_entry_scan` and
`lion_emit_all_keys` walk from the first leaf of THEIR key column to the first entry of the next one
(§24), and `IS NOT NULL`, verify and stats walk the leftmost leaf and then the right links, so their
output is in key order for an ordered opclass.

EXPLAIN ANALYZE reports **Directory Pages Read**, the leaves and internal pages the node read, which
is what `test/sql/directory.sql` uses to prove that a thousand-value IN list costs one pass over the
leaves its values live on rather than a descent each.

### Planner

When the grouped column's index is `ordered`, the GROUP BY is a single column driven from that index
(not the IN-list driver, not two columns, not a pinned single group), the table is not partitioned
and the column is **NOT NULL**, the CustomPath gets pathkeys for the group column, so an
`ORDER BY <group col>` above it needs no Sort.

Three conditions are subtler than they look:

- the NOT NULL requirement is not conservatism: the reserved NULL entry sorts FIRST and `ORDER BY
  col` means NULLS LAST, so a nullable column would be claimed in an order the node does not produce.
- the pathkeys are built for ASCENDING order from the KEY TYPE's own `<`, and not from the query's
  own `SortGroupClause.sortop`: `standard_qp_callback()` rewrites the GROUP BY clause's sort
  operators to match the query's ORDER BY when it can, so `ORDER BY k DESC` hands back a DESCENDING
  clause and building pathkeys from it would claim an order the node does not produce. (That bug was
  real and `test/sql/directory.sql` pins the `DESC` plan.)
- and the index's ordering must BE the key type's default btree ordering, not merely some ordering,
  because that is the order an `ORDER BY` asks for. A partitioned table gets none: each partition is
  ordered, but the Finalize HashAggregate on top destroys it.

### What goes away

`nbuckets`, `lion_bucket_of`, bucket sizing in ambuild, the bucket-chain warning and
`LION_BUCKET_PAGES_WARN`, `lion_clamp_buckets`, `lion_bucket_nentries`. The `buckets` reloption is
still accepted and ignored, with a NOTICE ("buckets is ignored since format 4") when it is SET - not
when the relcache reads it back. `max_bucket_pages`, `nbuckets` and `bucket_pages` in
`lion_index_stats()` become `directory_height`, `leaf_pages`, `internal_pages` and `ordered`. The
§17 cardinality guard's insert-side estimate becomes "the entries on this leaf times the number of
leaves" instead of "the entries in this bucket times the bucket count"; both are crude by design.

### Locking summary

Directory pages: nbtree's rules - no coupling downwards, move right when the high key no longer
exceeds the search key, splits hold left, then right, then the old right sibling, then the meta page,
and an ascent holds the child while it locks the parent. The insert path's guard (Operations,
above) adds only more left-to-right holding at the leaf level: the leaf a spanning prefix run is
entered from stays locked while the writer couples rightwards through the run and places the new
entry, splitting it if need be. Posting pages: the same rules again (§22),
with no old-right-sibling and no meta page in the record, and with all WRITERS of one key serialised
by the very directory leaf below. The lock order directory page → posting page is preserved, and the one place that used to break it
- VACUUM re-finding a moved entry - is done with nothing held. VACUUM's two-pass protocol addresses
the LEAF that holds the entry where it used to address the bucket head.

### Format

LION_VERSION 4 at the time this section was written; §22 bumped it to 5 in the next wave, and a
version 4 index is refused with the same REINDEX hint for the reason given there. verify() checks
the tree level
by level from the leaves up: page kinds and level numbers, sibling links, the high key present iff
the page is not rightmost, keys strictly increasing within a page and the last key below the high
key, the high key of a page not above the first key of the next, every downlink of a level naming
exactly the pages of the level below in that order (which is "every leaf reachable from the root
exactly once and the leaf right-link chain equals the in-order sequence"), the leftmost downlink of
each level being minus infinity, each separator at or below its child's own first key, each child's
high key at or below the next separator, and INCOMPLETE_SPLIT pages reported as a WARNING with the
repair hint. At the leaf level it also checks that no two entries of one prefix run - the same
column, kind, comparison and hash, across page boundaries too - are equal under the opclass
equality: two entries for one key would leave the rows of the one no lookup returns uncounted, and
the ordering checks alone only catch byte-identical twins (`directory.sql` §13).

### Not done in this version

Leaf deletion and page reclaim for an empty leaf (nbtree's half-dead protocol); a backward scan
(`leftlink` exists on DIRECTORY pages and verify() checks it, but nothing reads it yet - a future
`amgettuple` will; §22 turned out not to need it, and posting pages therefore keep no left link at
all); parallel build; and online deduplication of a prefix run that spans pages, which an opclass
with a comparison coarser than its equality could in principle produce.

## 22. Per-key posting tree (format version 5, implemented)

Replaces the linked chain of container pages per CHAIN entry (§4) with a B-tree over container keys
(ckey), GIN's posting-tree shape.

Why. A chain can only be walked. Every intersection therefore streams the dense sets in full: the
3-column AND at 5M rows visits 570 containers for a 590-row answer, and dirty-heap grouping
re-walks chains per group. With a tree, the AND is driven by the most selective set: for each of
its containers, the other sets are probed by ckey (descend, or step right from the last position).
Cost tracks the selective side. Inserts into the middle of a chain stop being a linear walk from the
head (the churn tests' mid-chain inserts).

Structure. The entry's `head` is the posting-tree ROOT (a container page when the tree is one page:
no separate root format, exactly as GIN). Container pages keep their layout (items sorted by first
ckey, minckey/maxckey, owner stamps, growth slack, DELETED marking) and use the `level` §21 had
already put in the special area: leaves are level 0, internal posting pages level 1 and up. They
carry LION_PAGE_CONTAINER like the leaves - the level is what tells the two apart - so the owner
check, the DELETED marking and the leak sweep apply to them unchanged. Right links stay at the leaf
level, so every sequential walk (scans, single-set counts, VACUUM) is what it was: the leaves are
still one rightlinked list in ckey order. **`leftlink` stays unused on posting pages**, which is
what keeps a split inside the four buffers a GenericXLog record allows.

An internal page's items are `LionPostingPivot { uint32 ckey; BlockNumber child; }`, 8 bytes, so a
page holds 679 of them. The convention is §21's, not GIN's: `ckey` is a SEPARATOR - a lower bound on
the child's subtree - and the FIRST item of every non-rightmost internal page is its HIGH KEY, a
strict upper bound with `child` = InvalidBlockNumber. Separators are non-decreasing rather than
strictly increasing (see the repair below). The leftmost downlink of a level is 0, which is minus
infinity for an unsigned ckey. **Leaves carry no high key** - they hold containers, not pivots - and
use `maxckey` instead; the next paragraph is why that is enough.

**Writers of one key serialise on that key's directory leaf**, and everything below rests on it.
Every path that changes a posting tree - aminsert, an INLINE spill, VACUUM's apply and regrow steps -
holds the directory leaf that carries the entry EXCLUSIVE while it does (§5, §11, §21), and VACUUM
re-validates that the entry is still on the leaf it holds before every write. So no split of a
posting tree can be in flight while another WRITER descends it, and a write descent needs no
move-right at the leaf level at all: the separators say exactly which leaf owns a container key,
which is also what keeps "a leaf's keys are all below its right sibling's" true when the key lands
in a gap. Only READERS race with splits, and a reader that lands on a leaf whose maxckey no longer
reaches its key moves right, which is where the split put the items.

Operations.
- **Descent** by ckey with SHARE locks, releasing the parent BEFORE locking the child - *not* lock
  coupling, which is a deviation from this section's first draft and the same one §21 records: a
  split holds the child while it locks the parent, so coupling downwards would close a cycle buffer
  locks have no detector for. The leaf is taken in the caller's mode directly, so no lock is
  upgraded. `lion_chain_find_page()` is that descent (`lion_posting_search()` in the new
  `lion_posting.c`); the tail hint stays for appends and is now decided with the EXCLUSIVE lock the
  insert needs anyway, without descending at all, when the tail is still the rightmost leaf.
- **Leaf split**: as today (items at and after the insert position move to a brand new page N linked
  right; when the new items still do not fit on P they get a second new page M linked between them),
  plus a downlink insert into the parent. Each new page's LEFT neighbour is flagged
  LION_PAGE_INCOMPLETE_SPLIT in the split record and held EXCLUSIVE until its downlink is in and the
  flag cleared, so a P → M → N split flags both P and M and inserts M's downlink first - a descent
  cannot reach M before M has one. Records: (P, M, N, entry leaf) = 4 buffers for the split, one for
  each downlink, one tiny one for each flag. Injection point `lion-posting-split-incomplete` fires
  between the first record and the downlink; `test/recovery/run.sh` phase 1d crashes the server there
  and proves the next writer's descent repairs it.
- **Root split is a PUSH-DOWN**, and this is the deviation that matters most from the first draft,
  which had it allocate a new root and rewrite the entry's `head`. The root block never moves: its
  items go to a brand new child and the root block itself becomes the level above, holding one
  downlink to that child (nbtree and GIN both keep their root in place). Three things fall out of
  it. `head` never changes, so the entry is never rewritten for a root split and the four-buffer
  budget is never the binding constraint - the push-down is (root, child, entry leaf) = 3, and the
  entry is only there because `tail` moves. "A head block is never recycled as a head" (§18) stays
  true without any new rule, so owner_head remains the identity a reader holding nothing but a head
  block can validate against. And the child is a verbatim copy of the old root, so the operation
  that overflowed simply runs again at the same offset on the child - the root lock is dropped for
  that window, because the child's own split has to take the root to insert ITS downlink and buffer
  locks are not reentrant, which is safe precisely because writers of one key serialise.
- **VACUUM**: the leaves are visited in ckey order via right links exactly as before, with a cleanup
  lock on every one of them (§11); pass 2 starts by DESCENDING from `head` to the leftmost leaf,
  and the descent takes a cleanup lock on every page it passes, the root first
  (`lion_vacuum_descend()`). *(Deviation from this section's first version, which walked straight
  to the leftmost leaf and said internal pages "are NOT cleanup-locked on the ordinary walk and need
  not be". They hold no TIDs, but the root of a one-page set is a LEAF a reader may have copied
  containers from, and the push-down below turns it into an internal page with those containers on
  a child VACUUM would then clean without ever asking for the reader's pin - §11 and
  `test/isolation/count_root_pushdown_race.spec`.)* Internal pages are never empty while the set
  lives, so the leak sweep leaves them alone. Freeing a whole set frees its internal pages too,
  bottom up, under ConditionalLockBufferForCleanup like the leaves. Internal pages are never
  deleted while the set lives (documented limitation), and neither are empty leaves - both wait for
  the whole set to go, as §18 already said of mid-chain pages.
  A root that is pushed down while VACUUM is walking it: VACUUM finds a page that is no longer a
  leaf, and descends again. That is safe for the reason §11's split hole is safe - the cleanup lock
  it holds on the old root waited for every pin on the page those containers came from, so no
  reader can hold a stale copy of them, and the page they moved to did not exist a moment ago.
  **An internal page's own split changes nothing here**: it moves pivots, not TIDs, to a brand new
  page at the same level, and a page's level never changes except the root's; the leftmost downlink
  of every level stays where the descent looks for it. An INTERNAL root's push-down likewise moves
  only pivots.
- **Cursors** (`LionSetCursor`) gain `seek(ckey)`: from the current leaf, step right while the leaf's
  maxckey is below the target and the walk is short (`LION_POSTING_SEEK_STEPS` = 2 pages, about
  where a descent's `height` reads and binary searches become cheaper), otherwise descend from the
  root; then position at the first item that can hold the target. A sparse segment covers a RANGE of
  container keys, so a seek into one skips its PAIRS rather than the item. A materialized set is an
  array and is binary-searched. *(Deviation: an INLINE set is NOT binary-searched, as the first draft
  said. An inline payload is a sequence of items packed without padding and carries no offsets to
  search; the seek skips forward over item headers instead, which costs what the sequential walk it
  replaces costs - one forward pass over the payload - and an offset index built to allow a binary
  search would have to make that very pass to build itself.)*
- **The merge is a leapfrog join.** One source is walked sequentially and the others are probed at
  the container keys it produces: the driver is the positive source with the fewest members, read
  off the entries' own `ntids`, and only the driver steps past a container key that has been
  counted - the others are left standing and are sought forward on the next round. Leaving them
  standing is what makes the probe a probe: stepping every source by one first would cost each of
  them a container at `key + 1` that the seek is about to skip anyway. The same holds inside an AND
  node of the expression evaluator (§17), where only the first child steps. Union and single-set
  counting keep the sequential walk.
  **Since §25 the probes are ordered and abandoned early**: the non-driver sources are sorted by
  ascending members (the same `ntids`), and a container key is abandoned - the remaining sources
  neither sought nor read - the moment the running intersection empties or a seek lands past the
  target. That is the executor half of the open item below, and it is measured there.
- **The §9 pin discipline is unchanged.** A leaf stays pinned until the container taken from it has
  passed the visibility-map check, and a seek releases the previous leaf's pin only at that same
  point: the two callers of `seek` are the merge's "this container key is missing from some set"
  branch and an AND node's wind-forward, which are exactly the two places that used to call
  `lion_ecursor_next()` for the same reason - nothing of those container keys reaches the visibility
  map, so no answer rests on them.
  **The early exit of §25 does not touch it either, for the same reason and one more**: a container
  key that is abandoned contributes nothing to the answer, so no visibility-map question is asked
  about it and there is nothing to discharge for any page it touched; and the sources that were not
  sought are not stepped, so they simply KEEP the pins they were standing on. A pin too many never
  makes a count wrong - it only makes VACUUM wait.
- **Owner validation (§18) applies to internal pages as well**, and to the LEVEL: a leaf's right link
  always names another leaf, so a page above level 0 reached through one is treated exactly as a page
  whose owner no longer matches - the end of the set.
- **Bulk build**: leaves are written left to right as before, and the internal levels are built
  bottom-up in the same pass, one open page per level, through the bulk-write API (nbtree's
  `_bt_buildadd` shape, which is what §21's directory build already does). A set that fits one page
  has no internal level and its single leaf IS the head. The moment a second leaf is needed the
  build reserves a block for the ROOT and re-stamps the first leaf with it - the first leaf's image
  is still in memory, and every page of a set has to carry the root's block as its owner (§18).

### Cost model (`lion_cost_count_rel()`)

The leapfrog changes two terms, and both say the same thing: only the DRIVER is read end to end.

- **Containers.** An AND source's containers term is the selective source's container count times
  the number of sources (probes), instead of the sum of all sources' containers. OR leaves and an
  IN list that drives the groups keep their own term - neither is an AND source - and the GROUP BY
  driver's term is unchanged, because what changed in the executor is the intersection of the WHERE
  clauses and nothing else.
- **Pages.** The driver pays its whole share of the index's container pages, as every source did
  before the posting tree. Every other source pays only for the pages its probes touch, and never
  for more than that share: it is sought, not walked.

Which source drives is decided here as the executor decides it, from the MEMBERS (`lion_run_merge()`
sums the entries' `ntids`) and not from the containers - a union of k sets lies in up to k times as
many containers as it has container KEYS, and counting those would hand the merge to the wrong
source. The sources are the ones the executor merges: one per OR restriction (a union is ONE source,
§19), one per positive clause outside them, and none for an IN list that drives the groups, which is
not intersected with anything (§15). What the driver then costs the others is its container KEYS, of
which there are never more than the heap has.

**How many pages a probed source reads.** One seek is a descent - one internal page per level plus
the leaf the container key lives on - or, when the key is a page or two ahead, a walk right, which
the seek takes only while it is no dearer than the descent it saves (`LION_POSTING_SEEK_STEPS`, 2
pages). So

    pages = Min(walk, probes x (height + 1))

where `walk` is the source's share of the index's container pages, what it cost before this section.
`height` is not stored anywhere the planner can reach - the meta page carries the DIRECTORY's
height, not a per-key posting tree's - so it is derived from the fanout the tree is built with (679
pivots to a page), which is exact for a bulk-built tree. A probed source is charged at the
interpolated page cost of `lion_heap_page_cost()` against its own index's size, as §15's IN list
lookups are, so a probe into an index far larger than the cache is still random I/O.

Measured against the executor, counting buffer accesses on the benchmark's one-million-row `fact`
(release build, 2026-09-22): `c2 = 1` walks its 151 container pages and touches **154** buffers.
Probed by `c1m = 12345`, which has one container key, it touches **2** - the root and the leaf, which
is where `height + 1` comes from. Probed by `c20k = 77`, which has a container at about 40 of the
heap's 301 container keys, it touches **118**, against the 100 this charges and the 154 a walk
takes: 40 descents plus the page or so of stepping each, since 40 keys leave a gap of under four
leaves and the seek walks two of them before it gives up and descends. The estimate is therefore
about right at both ends of the range and a little optimistic in the middle, where the leaves the
seek crosses and abandons are what it does not count.

An IN list inside an AND is k sets sought k times over, and an OR leaf is sought like any other set
(`lion_ecursor_seek()` descends into each arm), so the bound applies per set of the source. A GROUP
BY probes the WHERE sets once per group, so `probes` is counted over all the groups together - which
is many times more probes than a WHERE set has pages, so the formula charges one read of each WHERE
set and a grouped count is priced exactly as it was. That is also what it really costs: the sets a
GROUP BY intersects with every group are copied out of the index on their second use and probed in
memory after that (§9), so nothing but the first read is ever paid.

**Measured** (2026-09-22, 1M rows of the benchmark's `fact`, 19231 heap pages all-visible, release
build, nothing disabled - which plan the model picks IS the measurement; times are the median of
five `EXPLAIN ANALYZE` runs of the node and of the best plan without it):

    count(*) WHERE ...                     cost before -> after   chosen      node / other ms
    c20k = 77 AND c200 = 17 AND c2 = 1       168.8 -> 117.6       BitmapAnd     0.25 / 0.88
    c20k = 77 AND c2 = 1                     161.3 -> 110.1       node          0.25 / 0.18
    c200 = 17 AND c20 = 3 AND c2 = 1         186.2 -> 186.2       node          0.51 / 4.41
    c200 = 17 AND c2 = 1                     165.7 -> 165.7       node          0.60 / 9.75
    c200 = 17                                  8.8 ->   8.8       node          0.15 / 0.43
    c200 IN (17,18,19) AND c20 IN (3,4,5)    733.5 -> 733.5       node          2.19 / 9.73
    c200 = 17 OR c20 = 3                     165.6 -> 165.6       node          0.41 / 26.2
    GROUP BY c200                            906.5 -> 906.5       node          3.98 / 87.0
    GROUP BY c20 WHERE c200 = 17             335.9 -> 335.9       node          3.16 / 8.33

Only the two ANDs with a SELECTIVE source move, by a third; the IN lists, the OR, the NULL shapes,
both GROUP BY forms and the two-column grouping are priced to the cent as they were, because in
every one of them the driver is either the only source or has a container at nearly every container
key. The quick benchmark at one and five million rows confirms it from the other side: no case
changes plan and every case is at its baseline within run-to-run noise.

### The open item: the node is charged for pages, its competitor for tuples

`c20k = 77 AND c200 = 17 AND c2 = 1` is still NOT the node's, and it should be: 117.6 against the
BitmapAnd's 62.1, for a count the node answers in 0.25 ms against 0.88. The remaining gap is not the
count of pages - 100 against the 118 buffer accesses measured above - it is what a page is worth
here. The node's 100 pages are a resident part of a 306-page index (`ix_c2` is read by every query
that touches `c2`), charged at seq_page_cost, which stands for a page read from a DEVICE; what the
BitmapAnd is charged is CPU per TID, 4957 of them at cpu_index_tuple_cost, for work that measurably
takes three times longer. Per microsecond of real time the node is charged about thirty times what
its competitor is, and no honest count of pages closes that.

Two ways out, neither taken here:

- **the executor could stop reading those pages.** DONE in §25's wave, and it did what this said it
  would: probing in selectivity order and abandoning a container key as soon as the accumulator
  empties takes this query from **112 buffer accesses to 11** at one million rows, and 0.095 ms to
  0.040 ms. What it did NOT do is close the gap in the ESTIMATE, and §25's "also" item records the
  experiment that says why: a discount built from a survival fraction per level is right when the
  columns are independent and ten times optimistic when they are not, and nothing
  `lion_cost_count_rel()` can see tells the two apart. So `pages = Min(walk, probes x (height + 1))`
  stands, as an upper bound the executor can now beat.
- **or the model could price a resident index page as a buffer hit.** `lion_heap_page_cost()`
  already argues residency from `effective_cache_size`, but its floor is seq_page_cost, because for
  HEAP pages the competing plan reads the same pages and the comparison is fair. For the container
  pages of a small hot index it is not fair, and a third rung below seq_page_cost would say so. It
  would have to apply to walked pages as much as to probed ones, which moves every estimate in this
  model, so it needs its own pass over every pin in `test/sql/pushdown.sql` - the 20000-group
  refusal of §10 and the 200x20 refusal of §20 are the ones to watch, since both are refusals the
  node deserves.

Format: LION_VERSION 5. *(Deviation: this section planned to share §21's version 4, because the two
were meant to land in one wave; §21 shipped first, so the bump is separate. The page HEADER did not
have to change - §21 had already given container pages a `level` - but a version 4 posting set of
more than one page is a flat rightlinked chain with no root above it, which this code cannot descend
and therefore cannot write to. Opening one is the existing ERROR with the existing REINDEX hint.)*

verify(): per key, the tree is checked level by level from the leaves up, exactly as §21's directory
is. Page kinds, levels and owner stamps; the leaf right-link chain and ascending ckeys within and
across leaves, with minckey/maxckey describing the items; on internal pages a high key present iff
the page is not rightmost, separators non-decreasing and below the high key, the leftmost downlink of
each level being minus infinity, every downlink of a level naming exactly the pages of the level
below in that order - which is the same statement as "the leaf right-link chain equals the in-order
leaf sequence" - each separator at or below its child's own first container key, and each child's
own upper bound below the next separator. INCOMPLETE_SPLIT pages are a WARNING with the repair hint,
as §21's are. `lion_index_stats()` gains `posting_internal_pages` and `max_posting_height`;
`container_pages` counts leaves only. `lion_index_posting_root(idx, key)` is a test helper that
returns one key's root block, which is how the regression test asserts that a root split did not
move it.

### The one shape the regression suite cannot reach

A split of an INTERNAL posting page. A page holds 679 downlinks, so an internal split needs a key
with 680 leaves, and a leaf covers at least one container key of 64 heap blocks: about 340 MB of
heap for one key, which `make installcheck` has no business building. `test/sql/posting_tree.sql`
says so where it stops, and the path was exercised by hand instead:

    CREATE TABLE bigtree (id int, k int, pad char(60));    -- ~100-byte rows, so 64 heap blocks
    INSERT INTO bigtree SELECT i, i % 2, '' FROM generate_series(1, 3600000) i;   -- hold ~5000 rows

3.6M rows of about 100 bytes give 44,500 heap pages and 695 container keys, and two alternating keys
put 2,500+ members at each of them - a BITSET, so exactly one container per LEAF, 694 leaves per
key. Built by CREATE INDEX and grown from empty by 3.6M inserts, the two come out identical and both
verify clean: **1388 leaves, 6 internal pages, height 2** (per key: a root, two level-1 pages
because 694 downlinks do not fit on one, and its leaves). That is the internal split, the level-1
root push-down and the bulk build's second internal level, all three.

### What §21 left §22 (written after §21 was implemented, kept for the record)

- **The entry's `head` is written under the leaf's EXCLUSIVE lock, in the same GenericXLog record as
  the page that made it change** - which is exactly what a posting-tree root split needs. Every
  writer of a key holds the directory LEAF that carries its entry from the descent to the last
  record (`lion_insert_one()` takes it EXCLUSIVE and releases it at the end; VACUUM takes it per
  window). `lion_put_entry()` / `lion_replace_entry()` are the only ways `head` ever changes, they
  take the caller's open `GenericXLogState`, and an entry never changes size once it is a CHAIN
  entry, so the write cannot fail. In the end the root push-down made this moot for the root split
  itself - `head` never changes at all - but it is still what lets `tail` travel with the same
  record, and it is the property that makes writers of a key serialise.
- **The buffer budget is the thing to watch.** The directory's own split already uses all four
  buffers of a GenericXLog record (left, right, old right sibling, meta page), and the meta page is
  in there only to keep `dirpages` exact for the cost model. A posting-tree split that also wants
  the entry leaf has three buffers of its own left, which is why §21 put the "clear the incomplete
  flag" step in a third record rather than joining it to the parent insert: the same trick - an
  idempotent repair driven by a page flag - is what §22 uses, and it is cheaper than a custom
  resource manager. Posting pages keep no `leftlink`, which is what buys the fourth buffer for the
  entry leaf.
- **`lion_chain_find_page()` is already the only place a ckey is turned into a block**, and its two
  callers (`lion_insert_lock_chain_page()` and the verifier) pass the entry's `hash`/`head`/`tail`,
  so replacing the walk with a descent changed one function.
- **Container pages have a free `level` field** in the special area since §21 (the directory needed
  one and the special area grew to 32 bytes either way), so §22 needed no further format change to
  the page header - only the internal posting page's item layout.
- **The §11 proof text now says explicitly which pages are cleanup-locked and why** ("every page
  that can hold a TID"), so §22's internal posting pages fall under the same sentence: they hold no
  TIDs, and cleanup-locking them is optional rather than load-bearing. *(Wrong for the ROOT, which
  is a page that can hold TIDs until it is pushed down; see the VACUUM bullet above and §11. Kept as
  written because this list is the record of what §21 handed over.)*

Order of work. §21 first (it changes where entries live; the posting tree hangs off the entry and
is independent of the directory shape), §22 second. Multicolumn indexes
(`USING lion (a, b, c)`) come after both and mean one directory holding each column's keys as
independent posting sets (order-insensitive, like GIN); a composite-tuple key is deliberately not
offered: a query with one fixed shape is btree's job.

### Measured (2026-09-22, 1M rows, 12352 heap pages all-visible, assert build)

`fact(c2, c10, c200, c20k, c200_clustered)`, the same binary pair measured in one session. The
number that moves is CONTAINERS VISITED; the wall times at this scale are a few hundred
microseconds either way and are dominated by everything but the merge.

    count(*) WHERE ...                        containers before -> after   ms before -> after
    c10 = 3 AND c200 = 17 AND c2 = 1                579 ->  579            0.414 -> 0.435
    c10 = 3 AND c200 = 17                           386 ->  386            0.315 -> 0.319
    c20k = 77 AND c200 = 17 AND c2 = 1              424 ->  140            0.243 -> 0.203
    c20k = 77 AND c200 = 17                         231 ->   93            0.149 -> 0.149
    c200_clustered = 7 AND c2 = 1                    11 ->    5            0.157 -> 0.158
    c200 = 17 (one set)                             193 ->  193            0.197 -> 0.199
    GROUP BY c200                                 38600 -> 38600          12.40 -> 12.46

The three-column AND on UNCORRELATED dense columns cannot improve and does not: c2, c10 and c200
each have a container at every one of the heap's 193 container keys, so every key is common to all
three and 3 x 193 is the minimum any algorithm can read. The gain is the whole point of the section
and shows up exactly where the section said it would - when one source is SELECTIVE in container
keys: `c20k = 77` has 48 of the 193, and the AND goes from reading both dense sets in full to
probing them 48 times each. A clustered key is the extreme of the same thing. The wall times at this
scale are a few hundred microseconds either way and are dominated by everything but the merge; the
containers are what the section changes.

Writes and maintenance, same session:

    mid-chain insert of 20k rows      662 / 692 / 731 ms   ->   155 / 134 / 142 ms
    portfolio build, 8 indexes            8348 ms          ->       8221 ms  (163 -> 165 MB)
    VACUUM, portfolio (bench/vacuum_micro.sh)
        wall / index WAL / index records   764 ms / 59 MiB / 10155
                                       ->  760 ms / 59 MiB / 10245
    VACUUM, churn cycles 1..3          119 / 114 / 123 ms  ->   121 / 116 / 115 ms
    GROUP BY c200 on a 5% dirty heap        99.5 ms        ->       95.6 ms

The mid-chain insert is the write-side point of the section, and it is the largest single win here:
finding the page for a container key in the middle of a 277-page posting set used to be a walk from
the head - about 140 pages, each SHARE-locked, inside the window that holds the entry's directory
leaf - and is now a two-level descent. 4.8x, on a 4M-row table with a DELETE + VACUUM in the middle
of its id range and 20,000 rows inserted into the freed heap pages.

The index grows by 2 MB over the portfolio (163 -> 165 MB): one internal page per posting set with
more than one leaf. The build and the VACUUM are unmoved, which is what they should be - neither
does anything the section changed except write and walk one more page per multi-leaf set.

### §22 addendum: the entry-page retry re-checks the page type

VACUUM's pass 2 holds a container page's cleanup lock while it filters it, then needs the entry
page for the counters. If an insert holds the entry page, VACUUM lets the container page go (waiting
for a directory leaf while holding a container page would invert the lock order), takes the entry
page blocking, and retries the cleanup lock. In that unlocked window the insert that held the entry
page may have overflowed this one-page set and pushed its root down (§22), leaving an INTERNAL page
at the block VACUUM is about to re-filter. The retry therefore re-checks `LionPageIsPostingLeaf`
after reacquiring the cleanup lock and restarts the descent exactly as the first acquisition does;
filtering an internal page as a leaf would read pivots as containers (an assertion on a cassert
build, dead TIDs left behind on a release build). Injection points `lion-vacuum-page-filtered` and
`lion-vacuum-entry-busy` make the window deterministic; test/isolation/vacuum_retry_pushdown.spec
parks VACUUM on either side of it while an insert pushes the root down, and asserts that every dead
TID is gone afterwards (ntids = rows) and verify() is clean. Before the re-check the spec crashed the
backend.

### §21 addendum: binary coercion requires the same equality function

The cross-type probe shortcut (relabel a binary-coercible probe to the key type and use the key
type's own hash/equality/ordering) is taken only when the family's cross-type strategy-1 operator is
implemented by the same function as the key type's own strategy-1 operator; otherwise the family has
stated a different equality (e.g. `bpchar =~~= text` with text semantics on a bpchar index, where
'x' and 'x ' differ) and the probe walks the leaves with that equality. test/sql/directory.sql §18
shows the shortcut answering 100 where the family and the seqscan answer 0.

**Before any of the outcomes above, a value of the column's OWN type needs no resolution** - and
for a class declared on a POLYMORPHIC type the class's input type does not say what that is
(2026-09-25 review). `enum_ops` is FOR TYPE anyenum: a scan key names its member by that type
(`sk_subtype` = anyenum), but the elements of `m IN ('a', 'b')` are of the column's enum, because
the operator is polymorphic and the parser leaves the array as it is (`make_scalar_array_op()`).
`lion_probe_init()` took `keytype == opcintype` for the only way to say "own type", so every path
that probes with the ARRAY's element type - a plain index scan's set tree and its LIST batches
(§29.3, §29.4), a multicolumn bitmap scan (§24) - looked up an (anyenum, mood) member, found none
and raised "type mood cannot be compared with index"; only the single-column bitmap scan, which
probes with `sk_subtype`, worked. For a polymorphic class the own type is now also the column's
actual type, read from the key column (`LionState.typid`), with domains looked through on both
sides; a different enum is still not one (its OIDs mean nothing to this column) and still refused.
`lion_range_add()` had made the same step for range bounds since §28. test/sql/keytypes.sql runs
every scan shape against a sequential scan on an enum column, a list longer than a batch included.

## 23. Backlog (not urgent; ordered by when they should happen)

- **Prune on access in the count's heap recheck (PostgreSQL 19+) - implemented.** The argument
  that on-access VM setting keeps "a set container bit on an all-visible page is exactly one
  visible row", checked against 19's and 20devel's pruneheap.c, and the implementation notes, are
  in §11's last subsection ("On-access pruning sets the visibility map too"); the claim holds, so
  the pages core already cleans on access on 19 were never a correctness problem either.
  `lion_recheck_heap_heap()` calls `heap_page_prune_opt()` once per heap block it reads, with
  `rel_read_only` decided when the LionCount node starts (the SQL-callable counts pass false).
  Tests: test/sql/prune.sql (after HOT updates the first pushed-down count leaves the pages
  all-visible on 19+ and the second rechecks nothing; a GROUP BY meets pages an earlier group
  cleaned; exact answers everywhere) and test/isolation/count_prune_race.spec (a count parked
  with its container pinned while VACUUM waits for the pin and core and the count prune around
  it; exact answers). Limits: helps only HOT updates (non-indexed columns) on pages that are
  nearly full (pruning's own free-space heuristic); deletes and indexed-column updates leave
  LP_DEAD items and still need VACUUM. Measured motivation: the 2026-09-23 PostgreSQL 19 focused
  run had its dirty pages cleaned by the untimed correctness scan before timing (all but one heap
  block skipped via the VM; 1.95 ms vs 79.7 ms on 18 for the 5M dense count), so that run's
  "dirty" rows are clean measurements. Page-at-a-time visibility for dirty pages was considered
  alongside and deferred past v1: it re-implements HOT-chain visibility outside heapam for a gain
  on 16-18 and on non-HOT churn only.
- **Insert batching, only if the custom rmgr leaves hot-key throughput short.** A GIN-style pending
  list was considered and rejected: GIN's per-row cost is the number of extracted keys (30 posting
  trees per tsvector row), which batching amortises; ours is one posting-set update per scalar row,
  and the measured bottleneck is the generic-WAL page copy inside the lock window (unlogged control:
  643 → 868 tps only), which a pending list moves rather than removes. It would also put TIDs outside
  the pinned-page protocol of §9, forcing every count to scan and snapshot-check an unordered list
  (pg_roaring_index's count path pays exactly this) and adding a merge phase to VACUUM. If batching
  is still needed after the rmgr: per-key deferred appends kept inside the entry tuple (a small
  sorted TID tail merged into the containers on the next insert that finds it full, or by VACUUM),
  which stays inside the pinned-page protocol and keeps counts exact. Multi-key classes are the one
  place a real pending list might pay; revisit only with a measured tsvector ingestion case.
- **FK-side join pushdown: implemented, see §27.** `GROUP BY dim.attr` over a fact table joined on
  a lion-indexed FK column. The sketch here - per dimension group, the union of the member keys'
  posting sets ANDed with the fact filters - became a count per DIMENSION ROW emitted as a partial
  aggregate, with core's Finalize Agg doing the grouping: the join count is additive over dimension
  rows, so no per-group key set (and no memory bound on one) is needed, and the "subquery-produced
  key set" is the dimension side's own plan, run as the node's child.
- **`count(DISTINCT k)` in the pushdown: implemented, see §26.** Both shapes (`count(DISTINCT k)`
  over the WHERE, and per `GROUP BY g`) with the rules this item set: existence tests with early
  exit, NULLs never counted, the grouping-equality check, multi-key columns and partitioned tables
  declined, mixed target lists, HAVING through plan.qual, pairs charged for shape 2, and a
  distinct-count variant of count_vacuum_race (`count_distinct_vacuum_race.spec`). It was done
  before the FK-side join pushdown, not after it as first planned.
- **PGXN packaging (done 2026-09-23 up to the release steps; before the Citus/TimescaleDB work).**
  Distribution through the PostgreSQL Extension Network is how the two environments below will
  install it, so it comes first. Done:
  - `META.json` (PGXN Meta Spec 1.0.0; `validate_pgxn_meta` from PGXN::Meta::Validator 0.16 says
    OK, and CI runs it): distribution `pg_lion` 0.1.0, `release_status` unstable (prototype),
    license `postgresql`, `provides` pg_lion and pg_lion_citext, PostgreSQL >= 16.0.0 as a build
    and runtime prerequisite (the minimum since eb1579e, `src/lion_compat.h`), `citext` runtime-
    recommended and test-required (only the companion needs it, and its control file's
    `requires = 'pg_lion, citext'` enforces that at CREATE EXTENSION), resources and tags.
  - Versions: PGXN's semver `X.Y.0` is the extension version `X.Y`. The control files keep
    `default_version = '0.1'` and `pg_lion--0.1.sql`: renaming to 0.1.0 buys nothing before a
    first release and would break the scripts and sections that name `pg_lion--0.1.sql`
    (bench/run_container_bits_experiment.sh, §6, §8, §17) and `ALTER EXTENSION` on existing dev
    clusters. The next SQL change ships as `pg_lion--0.1--0.2.sql` with META.json 0.2.0; the CI
    dist job fails if a control file's version plus `.0` differs from META.json's.
  - `make dist`: `git archive` of the committed HEAD into `pg_lion-<version>.zip` (META.json,
    README, LICENSE, DESIGN, Makefile, control and SQL files, `src/`, `test/{sql,expected,isolation,
    unit,recovery}`, `test/rmgr-check.sh`, `dev.sh`; no `.local`, bench, review evidence or build
    products). Built and tested from the extracted archive, outside the checkout, against 16, 18,
    19beta4 and master from source and against the PGDG packages 16.15 and 19~beta3 (non-cassert,
    unpacked from the .debs): unit, installcheck and installcheck-rmgr all green. Two Makefile
    changes for packaged installs: `PG_CONFIG` falls back to `pg_config` on PATH when `.local/pg`
    does not exist, and the unit tests link `pkglibdir` first, because Debian keeps the server's
    own `libpgcommon.a` there and `libdir` holds libpq-dev's, which is of another major.
  - CI (`.github/workflows/ci.yml`): PGDG packages for 16, 17, 18 and 19 beta (the injection-point
    specs are skipped there, since packaged servers have no injection_points module); REL_19_STABLE
    and master from source with cassert and injection points, cached per upstream commit, failing
    if any spec is skipped, rmgr run with `wal_consistency_checking`; and a dist job (validate,
    `make dist`, build and test from the zip). Each server job runs unit, installcheck and
    installcheck-rmgr through `./dev.sh` with `LION_SOCK`/`LION_PORT`. Checked with actionlint
    1.7.7 and shellcheck 0.10 and the steps dry-run locally; not yet run on GitHub.
  - `pg_upgrade`: works across majors, and found a preload bug (§23 addendum below).
  Remaining:
  - The WAL resource manager id, before any release. `pg_lion.rmgr_id` (`lion_wal_init()`,
    `src/lion_wal.c`) defaults to `RM_EXPERIMENTAL_ID` (128), the id core sets aside for
    development: two extensions that both use it cannot be preloaded together. A release must
    reserve an id on https://wiki.postgresql.org/wiki/CustomWALResourceManagers, make it the
    default, and keep the GUC so a site can move out of a collision. Document the rule for
    changing it: WAL written under one id replays only under that id, so change it only after a
    clean shutdown with every standby and archive consumer caught up (pg_upgrade carries no WAL,
    so an upgrade may change it freely).
  - Publishing: a PGXN account and upload of the zip, `release_status` raised when the prototype
    label goes, the first CI run on GitHub, and a README install section (`pgxn install pg_lion`).
  - Version scripts: from 0.2, `pg_lion--0.1--0.2.sql` upgrade paths and `ALTER EXTENSION
    UPDATE`; format-version bumps that need REINDEX must say so in the upgrade script's NOTICE and
    in the release notes. (The seven pre-18 opclasses of the addendum below need no repair
    there: lionvalidate() accepts their hash functions on every major.)
  - ~~Hook coexistence in the matrix~~ - done 2026-09-23, see "Hook coexistence" below.
- **Citus and TimescaleDB compatibility (last, before any release).** These are the environments the
  extension is most likely to run in. Verify, with a test matrix run against each:
  - Citus: distributed and reference tables with lion indexes (CREATE INDEX propagation via the
    normal DDL path; the opclasses, reloptions and the citext companion must be creatable on
    workers); the count pushdown must run on the worker shard queries (the coordinator sees a
    per-shard `count`/`GROUP BY` fragment, which is our single-table shape) and must be safe under
    Citus's use of the `create_upper_paths_hook` and `set_rel_pathlist_hook` (chain, never replace);
    shard rebalancing (indexes on moved shards rebuilt correctly); `citus.enable_repartition_joins`;
    `citus_columnar` tables are a separate item, below.
  - citus_columnar — secondary; a bonus after bare Citus and TimescaleDB are verified (a table
    access method, not a planner layer). Two facts drive it: it has no
    visibility map and its MVCC is stripe-level, so the heap-skipping count is unavailable as is;
    and its TIDs are synthetic (stripe row numbers), with offsets far beyond the heap's per-page
    maximum, so the 9-bit offset encoding of §2 cannot represent them and lion_check_key_offset()
    errors at build. Two levels of support, in order: (1) correctness — a lion index on a columnar
    table must either be refused with a clear error or work through the ordinary bitmap path with
    a TID encoding chosen per table AM (a wider offset field for non-heap AMs; containers cover
    fewer "pages" then), while the count pushdown declines the table via the table-AM check the
    heap recheck already uses; (2) value — a count pushdown for columnar needs a visibility source
    in place of the VM: columnar's stripe metadata records fully visible stripes and stripes with
    deletions, which is a coarser equivalent; the §9 interlock would have to be re-derived against
    columnar's own vacuum before any stripe is counted without a row visit. Level (1) should hold
    before release (an index on a columnar table must not corrupt or error obscurely); level (2)
    is optional and decides whether lion is useful on columnar rather than tolerated.
    **Level (1) done (2026-09-23), as the refusal**: ambuild refuses every table AM whose routine
    is not the heap's, with a clear FEATURE_NOT_SUPPORTED error, and the count pushdown and the
    SQL counts decline or refuse such a table too; the rules, the reasons and the tests are in §2.
    Verified against a copied-heap test AM (`test/modules/lion_hooktest`), not against
    citus_columnar itself, which still has to be run once when the Citus matrix exists. The same
    rule applies to a TimescaleDB chunk kept in another table AM (its hypercore AM): the index
    fails to build on that chunk rather than skipping it, and whether that is the behaviour
    wanted there is for the TimescaleDB item below to decide.
  - TimescaleDB: hypertables (indexes created per chunk through Timescale's DDL hooks; the pushdown's
    partitioned-parent path in §16 sees a hypertable as an inheritance parent with chunks as
    children — confirm the AppendRelInfo mapping and the `rte->inh` handling; Timescale also installs
    planner hooks that must be chained with ours), compressed chunks (no lion index on compressed
    chunks; the node must decline or the planner must not offer it for those children — compressed
    chunks are a different table AM), continuous aggregates (their materialized hypertables are
    ordinary hypertables), `timescaledb.enable_chunk_append`, and retention drops (a dropped chunk
    takes its indexes with it; nothing to do but test). Time-bucketed columns are a natural lion
    key (`time_bucket(...)` as an expression index is NOT supported — only plain columns — so this
    means a stored bucket column; document that).
  - Both: hook chaining order (load order via shared_preload_libraries vs LOAD-on-first-use), the
    `pg_lion.*` GUC prefix under their GUC validation, EXPLAIN output through their custom nodes,
    parallel-plan interaction (we are parallel-unsafe; their planners must respect it), and the
    supported PostgreSQL major versions (they lag master; this decides which release to target).
- **Range/zone-map opclass family (last; limited value)**: bucket a continuous column into value ranges, one posting set per
  bucket; a range predicate becomes the sum of the fully covered buckets' cardinalities plus a heap
  recheck of the two edge buckets' rows; `GROUP BY width_bucket(...)` is a header read per bucket.
  ORDER BY is not served (bitmaps deliver heap order). A bitmap zone map, not a btree substitute.
- **Plain index scans: implemented, see §29.** `amgettuple` on every major (16 .. 20): the scan
  streams the bitmap path's own set algebra one container at a time, drops every pin between calls
  under an MVCC snapshot and keeps the batch's page pinned under any other (exclusion constraints
  become possible). Its follow-ups, in the order the user set on 2026-09-24:
  - a CustomScan that walks a BTREE on the ORDER BY column and tests each TID against lion's exact
    WHERE set, stopping at LIMIT n (PG16+, DESC and multi-column order from the btree): designed
    in §30 (`LionOrdered`), which builds its set from the scan's source of §29.8;
  - ordered lion scans (`amcanorder`, 18+): deferred, §29.8 records what they would take;
  - index-only scans (`amcanreturn`, §29.9) and backward scans (§29.10).
- **Range predicates on the scalar classes: see §28.** `<`, `<=`, `>=`, `>` and `BETWEEN` as a
  bounded walk of the sorted directory, in the bitmap scan and - bounding the driving entry walk -
  in the count pushdown. Its follow-ups, in order of value:
  - a range on a column that does NOT drive the count (`g, count(*) ... WHERE <range on k> GROUP BY
    g`, the commonest shape of all; a range under an OR; a range as an FK-join fact filter): either
    the range as a union SOURCE, built per container key into §15's bitset image so that no entry
    has to stay located, or §20's (g, k) pair loop with the pair counts summed per group; each with
    its own cost terms (entries in range per count, or |G| x entries in range pairs);
  - `min(k)` / `max(k)`: walk from either end of the column to the first entry with a visible row
    (§26's existence test), printing its key under the value-representation contract; max needs the
    backward walk §21's leftlink was kept for;
  - cross-type date/timestamp members (`datecol < now()`), with their equality;
  - carry the recheck batch across the entries of a range SUM, as §15's disjoint sum carries it
    across a list's sets: the entries' counts are only added up, so their dirty TIDs may be
    rechecked together, one visit per page per batch, instead of per entry (§28's measured 68,384
    visits for 37,133 pages, and a model that has to charge them);
  - a STABLE bound expression in the count pushdown (`ts > now() - interval '1 day'`, the commonest
    range of all): the node takes a Const or a Param, as for equality (§10), and this is neither.
    Evaluated once per scan like a Param it would mean the same thing - a stable function returns
    one answer per statement - so this is only a matter of widening `lion_is_value_expr()` to
    pseudo-constant expressions for RANGE clauses and EXPLAINing them. The bitmap scan already
    takes them: the executor evaluates a pseudo-constant index qual as a run-time key.

### §23 addendum: hook coexistence (verified 2026-09-23)

What pg_lion installs into the server, all of it from `_PG_init()` (lion_am.c, lion_wal.c):
`create_upper_paths_hook` and, since §30, `set_rel_pathlist_hook`, chained the same way (the
previous hook first), and nothing else among the hooks - no planner, executor, ProcessUtility or
object-access hook -; a custom WAL resource manager, only
while `shared_preload_libraries` is processed; its GUCs, with the `pg_lion` prefix reserved by
`MarkGUCPrefixReserved()`. Each hook saves the previous value and calls it FIRST, then adds its own
path (`lion_create_upper_paths()`, `lion_ordered_set_rel_pathlist()`), so whichever extension was loaded before pg_lion has already
added its paths when pg_lion adds its own, and an extension loaded after pg_lion reaches pg_lion
through its own chaining. Nothing is ever uninstalled: core has not unloaded a library since
PostgreSQL 15 (no `_PG_fini`), and `_PG_init()` runs once per process, so there is no reload-order
case beyond the two load orders. No chaining bug was found.

`make hookcheck` (test/hook-check.sh, in CI on every server job) checks it against
`test/modules/lion_hooktest`, a test-only module that chains the same hook, counts its calls,
notes whether a LionCount path was already in the grouped rel when the previous hook returned
(which tells which of the two hooks was installed first), registers a resource manager of its
own when preloaded (id 128 by default, pg_lion's default too) and reserves its own GUC prefix.
Against the dev cluster, restarted with the options on pg_ctl's command line:

- nothing preloaded, lion_hooktest LOADed and pg_lion then loaded on first use, by the planner in
  the middle of planning the first query over a lion index; and the other way round;
- `shared_preload_libraries = 'pg_lion,lion_hooktest'` and `'lion_hooktest,pg_lion'`, with
  `pg_lion.rmgr_id = 129`: both resource managers registered, an index built in rmgr mode, written,
  vacuumed and verified next to the other;
- in each of those four, the other hook runs on every query, sees pg_lion's path exactly when it
  was installed after pg_lion (6 times per test file, or 0), LionCount is in the plan for a plain
  count, a GROUP BY and a partitioned GROUP BY, the answers equal those with the pushdown off,
  and `SET pg_lion.<anything else>` and `SET lion_hooktest.<anything else>` are errors; since
  §30 the module chains `set_rel_pathlist_hook` as well, and its hook finds the LionOrdered path
  in the base rel exactly when pg_lion's was installed first (twice per test file, or 0), with the
  ordered answer equal to the one with `pg_lion.enable_ordered_scan` off;
- both preloaded on id 128: the postmaster refuses to start with core's `failed to register
  custom resource manager "lion_hooktest" with ID 128` / `Custom resource manager "pg_lion"
  already registered with the same ID` - the collision the `pg_lion.rmgr_id` GUC exists to move
  out of;
- `HOOKCHECK_FULL=1` also runs the whole regression and isolation suite in both preload orders
  (done once, on 18: green).

Green on 16, 17, 18, 19 and master. What this does not show is how Citus or TimescaleDB use the
hook - whether either replaces paths, or plans the query elsewhere, before pg_lion sees it; that
is still the Citus and TimescaleDB item's to test.

### §23 addendum: pg_upgrade with lion indexes (verified 2026-09-23)

pg_upgrade restores the catalogs with `pg_dump --binary-upgrade` and copies the index files as
they are. Nothing in the on-disk format (version 6: meta page, directory, entry and container
pages, WAL mode recorded per index) depends on the server major, so the new server's meta-page
check accepts them and no REINDEX is needed. Run 16 -> 19beta4 and 18 -> master (20devel), both
clusters with `shared_preload_libraries = 'pg_lion'`, over a 200k-row table with ten lion indexes
(int, text, citext, bool, date, bytea, int[], a multicolumn one, and one each built with
`wal_mode = rmgr` and `generic`), after deletes, VACUUM and a dirty update:

- `lion_index_verify(idx, true)` passes on every index after the upgrade, entry and TID totals
  are unchanged, and the LionCount answers (equality, GROUP BY, multicolumn, citext, arrays) are
  identical before and after. Inserts, deletes and VACUUM on the new server, then
  `lion_index_verify` again, a LionCount answer checked against a sequential scan, and a new
  index build all pass. Both WAL modes keep working: WAL does not cross pg_upgrade, so the
  resource manager id does not matter to it.
- Bug found and fixed: with the library preloaded, pg_upgrade failed at "Checking database user
  is the install user" with `access method "lion" does not exist`. The planner hooks run in every
  database, and `lion_get_am_oid()` looked the access method up with `missing_ok = false`, so any
  aggregate with a WHERE or GROUP BY in a database without the extension (template1 here) raised
  an ERROR. It failed cleanly, but it also broke every such database of a server that preloads
  pg_lion for rmgr mode. The lookup now returns InvalidOid there, and no index or operator
  matches; `test/sql/noextension.sql` covers it under installcheck-rmgr.
- Upgrading from 16 or 17 to 18 or later keeps the seven opclasses that `pg_lion--0.1.sql`
  creates through `lion_create_opclass_pre18()` (bool, bytea, date, timestamptz, xid, xid8, cid)
  on their pre-18 substitute hash functions (hashchar, hashvarlena, hashint4, timestamp_hash,
  hashint8), because the dump carries the old catalog rows. The indexes are correct: the
  substitutes compute the same hash, which is why core accepted them before 18. But `amvalidate()`
  reported those seven as invalid on the new server, since lionvalidate() accepted the substitutes
  only below 18, and the function cannot be changed in place: `ALTER OPERATOR FAMILY ... DROP
  FUNCTION 1` refuses because the opclass requires it. Fixed at merge: lionvalidate() now accepts
  the substitutes on every major (they are hash-identical), so an upgraded index validates as it
  did before the upgrade. 18 -> master has no such difference; amvalidate is clean there.

## 24. Multicolumn indexes: independent per-column key sets in one relation (format version 6, implemented)

`CREATE INDEX ON t USING lion (a, b, c)` builds one relation whose directory holds, for each
column, that column's keys as independent posting sets. It is order-insensitive by construction: a
query on any subset of the columns intersects the matching columns' posting sets, exactly as separate
single-column indexes would, and it is GIN's multicolumn model (`amcanmulticol = true`). A composite
tuple key is deliberately NOT offered: a query with one fixed shape is btree's job.

Why one relation rather than n indexes: one build pass over the heap instead of n (n tuplesort
inputs from one scan), one VACUUM pass, one relcache entry and one planner IndexOptInfo, and a natural
home for the count pushdown to find every column's posting sets without matching n indexes.

**Directory.** The directory key is (column number, key): the directory comparator compares attno
first, and every entry tuple carries its attno in a `uint16` at offset 20 of the header, where the
32 bytes had four of alignment padding - so the header did not grow and no item on any page moved.
Reserved NULL and EMPTY entries exist per column. The leading attno term is compared before any
opclass function is called, which is what lets one descent walk past the entries of columns whose
key type it knows nothing about; the minus-infinity downlink of an internal page carries attno 0 and
still sorts below everything.

Each column has its own opclass, so the cached state became two structs: `LionState` is ONE key
column - key type, collation, hash, equality, ordering, `ordered`, multi-key extraction, plus its
attno and a pointer back to the index - and `LionIndexState` holds the meta page and the array of
them. `lion_get_state()` returns column 1, so every caller that predates this section compiles and
means what it meant. A `LionSearchKey` carries the column it is searching and that column's state,
which is what makes the comparator a pure function of (item, search key).

**Lookup**: (attno, key) descent. **Entry scan** for a GROUP BY: from the first entry of that attno
to the first entry of attno+1 - a descent to (attno, MINF), a bounded leaf walk that stops at the
first entry of the next column, still sorted for an ordered opclass. **IN lists**: per column.

**Build**: one heap scan feeding ONE TUPLESORT PER KEY COLUMN, drained into the shared directory in
attno order. *(Deviation from the first draft, which said "one tuplesort of (attno, kind, key, hash,
code)". One tuplesort needs one tuple descriptor and one sort operator per sort key, and the columns
of a multicolumn index have DIFFERENT key types, so there is no single `key` column to describe. n
sorts from one scan is what this section's own rationale asks for, it compares nothing across
columns, and it keeps each column's sort keys exactly what §21 chose for that column;
maintenance_work_mem is split between them.)* The directory order leads with the column, so the
columns' entry runs laid end to end are already sorted and the bottom-up level builder of §21 never
learns that more than one column exists.

**Insert**: one aminsert call produces n key inserts, each its own posting-set update under its own
entry's directory leaf, exactly as a multi-key opclass already produced several. **VACUUM** is
unchanged (posting sets are per entry); it carries the entry's attno next to its kind and key in the
identity check that survives a leaf split. **Stats**: `lion_index_stats()` returns ONE ROW PER KEY
COLUMN with a leading `attno`; entry- and posting-set-derived counters are that column's own and
relation-wide ones (the directory's shape, free and deleted pages) are repeated on every row. A
container page is attributed to a column through its owner_head stamp (§18) and the attno of the
entry that owns that head, accumulated per posting set and folded in at the end; a single-column
index skips that and attributes everything to column 1. **verify()** checks the attno of every entry
(in range, never below the entry before it), at most one reserved NULL and one reserved EMPTY entry
per column, and - with heapallindexed - every heap row under every key of every column.

**Scans (amgetbitmap)**: the scan keys arrive with `sk_attno`; each key resolves to its column, one
qual per column is answered (the most selective-looking one, as §15 already chose among several on
one column) and the columns are ANDed through the expression evaluator of `lion_count.c` - the same
evaluator a multi-key query's AND/OR tree goes through, because "the sets of a and the sets of b" is
an AND of set trees whatever produced them. A qual the sets cannot express is DROPPED and the TIDs
are marked for recheck, which is always correct because the bitmap heap scan re-applies the original
quals: `IS NOT NULL` (the complement of a set), a multi-key query the extractor answers with
LION_QMODE_ALL, and any second qual on a column. `IS NULL` per column is that column's reserved
entry.

**`amoptionalkey` is TRUE** (it was false). Without it the planner refuses any path that does not
constrain the FIRST index column, which for independent key sets is meaningless - `WHERE b = 1` on
`(a, b)` would not use the index at all. GIN sets it for the same reason. It also lets a PARTIAL
index whose predicate the query implies be scanned with no quals at all, and `liongetbitmap()`
answers that by emitting every row one key column holds, the reserved NULL entry included, with no
recheck.

**Count pushdown** (implemented, §10). `lion_find_roaring_index()` returns the index AND the key
column for a heap column - any key column, not just the first - and the WHERE clause / GROUP BY /
IN / OR / multi-key rules are all per (index, key column): every `opfamily[0]`, `opcintype[0]` and
`indexcollations[0]` became `[i]`, every lookup and entry scan its `_col` form, and
`lion_extract_query()` and the `datumCopy()` of a printed key take
`lion_index_column_state(index, attno)`. A query constraining a, b and c of one multicolumn index
becomes three posting-set sources from one index (the same as three indexes, but located through
one relcache entry), and a two-column GROUP BY may take BOTH columns from one index, which is this
section's natural use.

The column does NOT travel in `custom_private`. The executor derives it in `lion_open_relation()`
from the index it really opened and the clause's heap attnum (`lion_index_col_for()`), because a
partition's index may order the same columns differently from the parent's - and may number the
heap column differently too, which is resolved by name (`lion_heap_attno_in()`). So the planner has
only to be right about WHICH index. The one thing the executor cannot derive is the driving column
of a sum-over-all (§14), which has no group column at all: it is the column of the first
`IS NOT NULL` clause, which is exactly the clause the planner drove from. Two `IS NOT NULL` clauses
over one index made that distinction load-bearing - the driver's own clause is DROPPED and the
other column's NULL set must still be subtracted, and before this the two were told apart by the
index Oid alone.

The cost model treats each column's set as it treats a single-column index of that column: the
directory and container page terms are scaled by that column's share of the relation's entries,
estimated from the heap columns' n_distinct (the bullet in §10 has the formula and its
limitations). EXPLAIN prints `idx.col` for a multicolumn index and nothing for a single-column one.
The AM-side API for all of it is `lion_index_column_state(index, attno)`, the `attno` a
`LionPostingSet` now carries, and the `_col` forms of `lion_posting_set_lookup{,_many,_null}` and
`lion_entry_scan_begin`; the wrappers without a column are that column being 1.

One thing the node must NOT do is sum across columns. The disjoint-sum short-circuit of §15 counts
the entries of a union separately and adds them up, which is only the count of the union because
the entries of one SCALAR key column are disjoint; two COLUMNS of one index are not, and
`lion_sources_disjoint_sum()` refuses a source whose sets differ in attno for that reason. Nothing
in the pushdown can build such a source today - only an IN list is marked `disjoint`, and one
clause is one column - but an OR across two columns of one index is exactly the shape the guard is
about, and `test/sql/multicolumn.sql` pins it from both ends: the count equals the union and not
the sum, and EXPLAIN ANALYZE reports no posting set summed.

**Not in scope**: INCLUDE columns (`amcaninclude` is false and core refuses them) and per-column
reloptions. `amcanmulticol` limits: 32 columns (INDEX_MAX_KEYS, core's own error); a NULL in one
column does not affect the other columns' entries. *(Deviation: expression columns and duplicate
columns were listed as out of scope and turned out to need no code at all - the column number an
entry carries is the INDEX column's, never a heap attribute's - so both work and
`test/sql/multicolumn.sql` pins that rather than an error.)*

**Format**: LION_VERSION 6. Reading a version 5 index by treating attno 0 as 1 was considered and
rejected: the two formats would then be told apart by a field that means "column one" in one of them
and "no column at all" in the other. A version 5 index is refused with the same REINDEX hint every
earlier version gets.

### Measured (2026-09-22, 1M rows of the quick-v3 scalar heap, assert build)

One 3-column lion index on `(c2, c200, c20k)` against three single-column lion indexes on the same
columns and against three btrees, plus 10,000 inserts with each portfolio in place:

| portfolio | build | size | insert 10k rows |
| --- | --- | --- | --- |
| lion, one 3-column index | 3.01 s | 15532032 (15 MB) | 0.41 s |
| lion, three single-column indexes | 3.07 s | 15572992 (15 MB) | 0.41 s |
| btree, three single-column indexes | 1.48 s | 21135360 (20 MB) | 0.12 s |
| btree, one 3-column index | 1.99 s | 30711808 (29 MB) | - |

The two lion portfolios are the same index in one relation or in three: the same entries, the same
posting sets, the same page count to within 40 KB (three meta pages and three roots instead of one),
and neither the build nor the insert path can tell them apart, which is the point - the win is one
heap scan, one VACUUM, one relcache entry and one IndexOptInfo, not a denser index. What a 3-column
BTREE costs by comparison is the composite key it stores: twice the size of all three lion columns
together, for an index that answers one leading-column order.

*(Numbers from an assertion-enabled server, so they are comparable with each other and not with a
release build.)*

### Not done in this version

Per-column reloptions. A cost model that knows a column's real ENTRY count: the page terms are
scaled by the column's share of the heap columns' n_distinct, which is the right order of magnitude
for a scalar column and an underestimate for a multi-key one (one entry per lexeme, not per value),
and the directory HEIGHT is still the whole relation's. Reading a column's entry count off the meta
page - one counter per column, maintained by build and by insert - would remove both
approximations, and `lion_index_stats()` already computes it the expensive way.

## 25. Custom WAL resource manager (replacing generic WAL)

Why. Every page change is logged through GenericXLog today: register the buffer (an 8 KB image
copy), modify, then GenericXLogFinish() diffs image against page and writes the delta (or a full
image for a new page). Measured (§5): a single-TID insert moves ~48 KB of memory inside the lock
window, and an unlogged control lifts hot-key throughput only from 643 to 868 tps, so the copy, not
the logging, is the cost; mid-chain inserts carry 3x btree's WAL (quick-v3: 66 vs 25 MiB per 10k
rows at 1M). A custom resource manager logs the bytes that changed with no image, no diff, and a
lock hold that is one memcpy long. It also lets replay take the locks the §9/§11 argument needs on a
standby (a cleanup lock when applying a removal), which would let the standby count from the
visibility map again instead of rechecking every TID.

**Status: implemented** (`src/lion_wal.[ch]`, and every write path in the extension converted). The
resource manager is OPTIONAL and per index: a cluster can hold indexes of both kinds, a server
without the preload keeps writing generic WAL, and REINDEX moves an index from one to the other.
The subsections below say what was built, where it deviates from the paragraphs above, and what it
measured.

Registration. `RegisterCustomRmgr(rmid, &lion_rmgr)` from `_PG_init`; requires
`shared_preload_libraries = 'pg_lion'` (the API refuses registration after startup). rmid from a
GUC `pg_lion.rmgr_id` (default: one of the reserved custom ids; document the collision rule and how
to check `pg_get_wal_resource_managers()`). Without preload the extension keeps working on generic
WAL: the rmgr is optional, chosen at index creation and recorded on the meta page (`wal_mode`), so a
cluster can mix; an index created in rmgr mode refuses to be written by a backend whose server did
not register the rmgr (ERROR with the preload hint) but can still be read.

### The shim (`lion_wal.[ch]`, implemented)

Four calls, and every write path in the extension goes through them:

    LionWalState *lion_wal_begin(Relation index);
    Page  lion_wal_register_buffer(LionWalState *, Buffer, int flags);
    void  lion_wal_op(LionWalState *, Page, uint8 op, OffsetNumber off,
                      uint16 aux, const void *data, Size len);
    void  lion_wal_finish(LionWalState *, uint8 info);      /* and _abort() */

`lion_wal_register_buffer()` hands back the page to work on: the SCRATCH IMAGE
in generic mode, where everything is exactly as it was before this section, and
the BUFFER'S OWN PAGE in rmgr mode. `lion_wal_op()` describes one change and is
a no-op in generic mode, where the byte-wise diff describes it already. So a
call site is written once and reads the same in both modes; what forks is four
lines in one file. `lion_wal_register_data()` exists for record-level payload
and is used for the record header alone.

**The rule a call site must follow, and the deviation it forced.** In rmgr mode
`lion_wal_begin()` opens a CRITICAL SECTION, because the page is modified in
place and an ERROR between the first modification and XLogInsert() would leave a
page in shared buffers that no record describes. Everything fallible therefore
has to happen BEFORE the record opens. Three consequences, all of them
deviations from what this section assumed:

- `lion_new_buffer_xl()` is gone. Allocating a page is split into
  `lion_alloc_page()` (fallible: the free space map, or extending the relation)
  and `lion_wal_init_buffer()` (registers the page the caller already has with
  REGBUF_WILL_INIT and initialises it). Every split therefore works out how many
  pages it needs BEFORE it opens its record, and `lion_release_unused_page()`
  gives back one it turns out not to need.
- **A container split rehearses its own deletion.** `lion_split_and_place()`
  used to ask "do the new items still fit on P?" after deleting the moved ones
  from P, inside the record, and allocate the second new page M there if the
  answer was no. It now asks the same question of a private copy of the page
  that the deletion is rehearsed on, and allocates M before the record. A copy
  rather than arithmetic because PageIndexMultiDelete() compacts, and what a
  compaction recovers is exactly what the arithmetic would have to guess at. A
  split copies a page once; the hot path copies nothing at all, which is the
  whole point.
- `lion_wal_abort()` is legal only while nothing has been modified, which it
  asserts. All four callers qualify: each is a PageIndexTupleOverwrite() that
  tests before it writes. The "can't happen" `elog(ERROR)`s that remain inside a
  record become PANICs in rmgr mode, which is the honest answer for a page that
  changed under a lock this backend holds.

Nothing above changes generic mode, where GenericXLogFinish() opens its own
critical section as it always did.

### The record catalogue as implemented

An rmgr record's payload is, per registered block, a packed stream of eight-byte
operation headers `{op, off, aux, len}` each followed by `len` bytes. One
`lion_redo_apply()` executes the stream; the record TYPE (the top four bits of
`xl_info`) names the operation for `pg_waldump`/`pg_walinspect` and decides
which blocks replay takes a cleanup lock on. That is a deviation from this
section's first draft, which gave each record type a payload struct of its own:
twelve bespoke redo handlers would have been twelve places for a replay bug,
where an operation stream is one, and `wal_consistency_checking` checks the one.

The main data is a four-byte header, `{initmask, cleanupmask, nvisit}`: one bit
per block for the first two, and the number of barrier ranges that follow the
header (below, VACUUM_VISIT). It travels in the record's own data rather than a
block's so that redo can read it even when every block carries a full-page
image.

    record          info  buffers  payload
    ITEM_SET        0x00  1-2      CONTAINER_ADD or SPARSE_INS on the container
                                   page, MINMAX if a segment's range grew, and
                                   the entry tuple on the directory leaf.  The
                                   hot-key insert record: ~64 bytes.
    ITEM_REPLACE    0x10  2        DELTA (the ranges of the item at its allotted
                                   length that differ from what the page holds)
                                   + MINMAX + the entry.  98 B for the dense
                                   container that used to cost 1658; a rewrite
                                   that changes more than half the item falls
                                   back to REPLACE, which is the whole item at
                                   its allotted length with its slack zeroed.
    ITEM_ADD        0x20  1-2      DELETE (when replacing) + ADD per item +
                                   MINMAX + the entry.  Also every record of an
                                   INLINE spill.
    ITEM_DELETE     0x30  2        MULTIDEL + MINMAX + the entry.  CLEANUP LOCK
                                   on the container page.
    PAGE_INIT       0x40  1        INIT + SPECIAL.  Written by lion_new_buffer()
                                   alone, which nothing calls today: every page
                                   is initialised inside the record that links
                                   it in.  Kept because that is what the record
                                   type is for.
    SPLIT           0x50  2-4      Both halves as INIT + ADD/ADDMANY + SPECIAL,
                                   the old right sibling's SPECIAL, and META or
                                   the entry.  Covers the directory split, the
                                   root split, the container split and the
                                   posting-tree root push-down.
    DOWNLINK        0x60  1        ADD of one LionPostingPivot into a parent.
    SPLIT_CLEAR     0x70  1        FLAGS: clears LION_PAGE_INCOMPLETE_SPLIT.
    META            0x80  1        META.  Reserved: the meta page only ever
                                   changes inside a directory split today, so
                                   that record carries the META operation and
                                   this type is not written.
    VACUUM_PAGE     0x90  1-2      MULTIDEL + REPLACE per shrunk item + MINMAX +
                                   the entry, or a batch of REPLACEs on a
                                   directory leaf's INLINE entries.  CLEANUP
                                   LOCK on the page the TIDs leave.
    PAGE_DELETED    0xA0  1        DELETED with the safexid, which is LOGGED and
                                   not recomputed at redo.  CLEANUP LOCK.
    ENTRY           0xB0  1        ADD, REPLACE or MULTIDEL of entry tuples on a
                                   directory leaf.  CLEANUP LOCK when VACUUM
                                   deletes entries.
    VACUUM_VISIT    0xC0  0        The standby barrier on its own: the header,
                                   the relation's RelFileLocator and `nvisit`
                                   block ranges, no registered block.  Written
                                   only when VACUUM has collected
                                   `pg_lion.vacuum_barrier_ranges` (1024)
                                   ranges with no removal record to carry
                                   them.

The operations are: INIT, SPECIAL (the whole 32-byte page special area), ADD,
ADDMANY, REPLACE, DELTA, SETBYTES, MULTIDEL, DELETE_NC, DELETE, MINMAX, FLAGS,
META, DELETED, CONTAINER_ADD, SPARSE_INS.

**DELTA is what a rewrite of one item costs now**, and it is the one operation
whose payload is a function of the page as well as of the record. It says: the
item at `off` becomes `aux` bytes long, and here are the ranges
(`{uint16 at, uint16 len, bytes}`) in which it differs from the item that is
there now, taken as the BASE IMAGE truncated to `aux` bytes or zero-extended to
them. The writer computes those ranges against exactly that base before it
overwrites the item - `lion_wal_save_item()` copies the image, the
PageIndexTupleOverwrite() happens, `lion_wal_op_replace()` does the comparison -
so replay reconstructs the writer's item byte for byte and
`wal_consistency_checking` proves it does. Redo patches the item where it lies
when the length has not changed, and otherwise rebuilds it in a scratch buffer
and puts it back through the same PageIndexTupleOverwrite() the primary called,
which is what keeps the page identical when an item moves.

The allocated length may change, and that is the point of `aux`: an item that
has used up its growth slack (§4) is rewritten LONGER, and the new slack is
zeroed on both sides so it costs nothing in the delta. Every call site that
overwrites an item uses the pair - a container item, an entry tuple on a
directory leaf, VACUUM's shrunken items and INLINE payloads - and the fallback
is automatic: when the ranges would come to more than half the item, the
operation is dropped and a plain REPLACE takes its place, so an ARRAY that turns
into a BITSET still goes in whole. Nothing about masking changes, because the
bytes outside the named ranges are the bytes the page already had.

**Two operations log the writer's CALL rather than its bytes**, and that is
where the hot record's size comes from. Adding a member to a sorted ARRAY shifts
every element above it, so the bytes that change run to the end of the item -
up to 4 KB - while "add member `lo` to the item at offset `off`" is four.
CONTAINER_ADD calls `lion_container_add()` and SPARSE_INS calls
`lion_sparse_insert()` at redo, on the same item, which is deterministic for the
same reason every other operation is and is what `wal_consistency_checking`
checks. Everything else logs bytes.

**Offsets are always explicit.** A writer that calls PageAddItemExtended() with
InvalidOffsetNumber logs the offset it GOT, so replay never has to reproduce the
line-pointer search - a page whose unused line pointers differ would otherwise
diverge silently.

**A directory split logs BOTH halves**, which is a deviation: this section
preferred logging only the moved half, and that is what a CONTAINER split does
(P keeps its items, N and M get theirs). A directory split cannot, because both
halves are rebuilt from scratch and the only page the other half could be
derived from is the very page being re-initialised - whose pre-image a full-page
image would destroy. Both halves are registered REGBUF_WILL_INIT instead, so
neither pays for an image at all and the record is the page's live bytes.

**One record type is not enough to decide the cleanup lock, and
`wal_consistency_checking` did not find that one - reading the code did.**
VACUUM's REGROW step (§18: a container that grew while it was being filtered
and no longer fits its slot) re-places the item through the general machinery of
lion_pages.c, the same code an INSERT goes down, so the record it writes is an
ordinary ITEM_REPLACE or ITEM_ADD or SPLIT - and it removes TIDs. The fact is a
property of the CALLER, not of the operation: VACUUM holds a cleanup lock on
that page throughout. So `lion_wal_removal_begin()`/`_end()` bracket that window
and every record written inside it marks its FIRST registered block - which on
every one of those paths is the container page - as needing a cleanup lock at
redo. It travels that way rather than through five function signatures because
five signatures would have to carry a fact that only one caller in the tree
knows.

**The same holds for an INLINE spill that VACUUM's filtering causes, and there
the page TIDs leave is NOT the first block** (2026-09-22 review). A filtered
payload can outgrow its entry (a RUN that loses every other member becomes an
ARRAY), and pass 1 then spills it through `lion_entry_spill()` - the INSERT
path's code, which registers the posting set's new root first and the
directory leaf last, with no cleanup mark. When every changed entry of the leaf
spilled, the cleanup-marked VACUUM_PAGE record pass 1 had opened was abandoned
and the spill's records were the only ones that said the entry's dead TIDs had
left the leaf - so replay took no cleanup lock on it, and a standby reader
pinning that leaf for its INLINE copy was overtaken (`test/recovery/run.sh`
phase 3, "spill": 200 for 100). `lion_wal_removal_begin(Buffer also)` therefore
names the buffer as well: inside the window, every record marks its first block
AND any block that is `also`, and pass 1 wraps each spill in a window naming
the leaf. `lion_wal_finish()` then puts the cleanup blocks FIRST in the record
(existing pages before pages the record initialises), whatever order the writer
registered them in - the block ids are assigned only at XLogRegisterBuffer(),
so this is a permutation of the shim's state - which keeps §11's waiting rule
for the startup process without asking the shared placement code to know about
VACUUM. Every record VACUUM writes is now one of: VACUUM_PAGE/ITEM_DELETE/
PAGE_DELETED/ENTRY with its cleanup mark set by VACUUM itself, or a record of
the shared placement code written inside a removal window (regrow, spill); the
re-descent that finds a moved entry writes nothing.

**The standby BARRIER, and why the cleanup mask alone is not enough**
(2026-09-22 review). ambulkdelete cleanup-locks every page that can hold a TID
in chain order, and every page of each posting tree's descent (§11), whether or
not it removes anything there; that is what stops it from overtaking a reader
whose TIDs a split, a root push-down or a spill has since moved off the page it
pins. A page it locks and leaves unchanged writes NO record, so replay never
waited for a standby reader's pin on it, and the removal replay did wait on was
on the page the TIDs had moved TO. Phase 3 of `test/recovery/run.sh` got 50000
for 48548 (split) and 1000 for 500 (push-down) from exactly that. So VACUUM
hands every page it cleanup-locks without writing to it to
`lion_wal_visit()`, which keeps a list of block RANGES (consecutive blocks
merge), and `lion_wal_finish()` attaches the list to the next record whose
`cleanupmask` is not empty: `nvisit` `{uint32 start, uint32 count}` pairs right
after the header. Redo cleanup-locks every block of the list, one at a time and
releasing each, with nothing else held, before it takes the record's own
blocks - only while `InHotStandby`, since there is no reader to wait for in
crash recovery. This is nbtree's old XLOG_BTREE_VACUUM `lastBlockVacuumed`
in its economical form: no record of its own in the common case, eight bytes a
range, and the record type VACUUM_VISIT only for a list that grows past the
limit with nothing to ride on. Replay meets the barrier for a page before the
removal that could hurt a reader pinning it, because VACUUM visits the page
before it removes anything from a page right of it and the record is written
after the visit; pages visited after the last removal of an ambulkdelete are
dropped (every dead TID of the cycle has been removed by then, and a TID that
moved off a page later in the walk moved onto a page later still).

Redo. `lion_redo()` reads the header, applies the barrier if there is one, then
takes each registered block in the order the record lists it, with
RBM_ZERO_AND_LOCK for a block in `initmask` and `get_cleanup_lock = true` for a
block in `cleanupmask`, and applies that block's operation stream when the
action is BLK_NEEDS_REDO. Blocks that need a cleanup lock are always FIRST (the
shim reorders them, above), so the wait happens with no other buffer lock held
- §11's waiting rule, applied to the startup process.
The reverse order (container page, then directory leaf) is safe against standby
READERS because no reader ever holds two of these locks at once: a scan walks
the leaves one shared lock at a time, a descent releases the parent before
locking the child, and §11's rule for readers already forbids taking a directory
lock while holding a container page.

`rm_mask()` masks the page LSN and checksum, the hint bits, the free space
between pd_lower and pd_upper, and **the MAXALIGN padding after every item**.

That last one is the one thing this section got right for the wrong reason and
is worth spelling out, because `wal_consistency_checking` found it on its very
first run and nothing else would have. PageAddItemExtended() reserves
MAXALIGN(size) bytes and copies `size` of them; the bytes in between keep
whatever the page held there before. Core's own access methods do not care,
because a core index tuple is MAXALIGNed by index_form_tuple() and there is no
padding - but a lion entry tuple is MAXALIGN(header + key) plus a payload BYTE
COUNT, and a container item's size comes out of its header, so both routinely
end on an odd boundary. On the primary the padding holds whatever the page held;
on a standby the page may have arrived as a full-page image, whose HOLE is
restored as ZEROES, and the two pages then differ in bytes no record ever
described and no reader ever reads. The first failure was a 54-byte entry tuple
on a directory leaf, two bytes wide.

The growth SLACK inside an item (§4) is a different thing and is NOT masked: an
item is always logged at its ALLOCATED length with its slack already zeroed
(`lion_item_zero_slack()`), and the three operations that change an item without
rewriting it leave every byte outside the range they name untouched on both
sides. Masking it would hide a real divergence. Generic mode never had either
problem, because a byte-wise diff of the whole image covers padding and slack
alike - which is the general shape of what this wave traded away.

### Standby (implemented)

With cleanup locks taken at redo for removals, the hot-standby count uses the
visibility map again for rmgr-mode indexes. `lion_count.c` decides it per
COUNT, not per index: `cx.in_recovery` is now
`RecoveryInProgress() && !lion_sources_all_rmgr(...)`, because a container of an
intersection carries the dead TIDs of every source it came from and the
interlock has to hold for all of them - ONE generic-mode source loses it. The
argument is written out next to the code and is §9's with "VACUUM" replaced by
"the startup process": while the backend holds a pin on the page it took a
container from, replay cannot have removed a TID from that page, so it cannot
have replayed the heap record that set any of that container's heap pages
all-visible. §11's split hole closes the same way - a page is only ever linked
in immediately right of the page whose split created it, and replay applies
those records in the order the primary wrote them - **but only with the
barrier above**. *(Deviation, found by the 2026-09-22 review: this paragraph
first claimed the split hole closed by itself. It does not: the primary's
VACUUM passes the page the reader pins without writing to it, and replay used
to take no lock on it. Until the barrier existed an rmgr-mode standby could
count a dead row from a copy whose TIDs had moved; the shipped rule stays
"trust the map in rmgr mode" because phase 3 below now proves the barrier
under `wal_consistency_checking`. WAL written by a binary without the barrier
carries none, and a standby replaying such WAL keeps the old exposure until it
has replayed past the upgrade.)*

The reader pays for it the way a primary reader does: replay WAITS, which is a
recovery conflict resolved by `max_standby_streaming_delay` rather than a wrong
answer. `test/recovery/run.sh` phase 2 covers both halves in both modes - a
generic-mode standby must report `blocks_skipped = 0` and recheck every TID, an
rmgr-mode one must report `blocks_skipped > 0`, and the REPEATABLE READ standby
session whose rows the primary deletes and vacuums must be either preserved
(`hot_standby_feedback = on`) or cancelled by a recovery conflict, never
silently answered with a different number. **Phase 3** attacks the pin itself:
a standby reader parks at `lion-count-containers-pinned` (containers copied,
page pinned, map not consulted), the primary moves the copied TIDs off that
page - a leaf split, a root push-down, a VACUUM-made INLINE spill - and
VACUUMs, and the standby has `max_standby_streaming_delay = -1`. In rmgr mode
replay must be seen BLOCKED on the reader's pin (the startup process waiting
for a cleanup lock) and the reader, released then, must answer the count its
snapshot sees; in generic mode replay catches up and the reader must still be
right, because it rechecked every TID. The split case runs with
`pg_lion.vacuum_barrier_ranges = 1`, so its barrier travels in stand-alone
VACUUM_VISIT records; the push-down and spill cases carry it on removal
records. All three answered wrong before the barrier and the removal window of
the spill (50000/48548, 1000/500, 200/100).

### Registration and migration (implemented)

`RegisterCustomRmgr(lion_rmgr_id, &lion_rmgr)` from `_PG_init()`, and ONLY while
`process_shared_preload_libraries_in_progress` - so a library that is merely
`LOAD`ed does nothing. The `pg_lion.rmgr_id` GUC is PGC_POSTMASTER and is
defined in the same guarded block, because core refuses to define a
postmaster-level GUC after startup (pg_stat_statements returns from `_PG_init()`
at exactly this point and for exactly this reason). Default 128,
`RM_EXPERIMENTAL_ID`, which is the id the project reserves for development: a
release has to either reserve an id on the wiki page the core documentation
names or keep the GUC and say so. The GUC stays either way - the point of a
reserved range is that a site can move out of a collision - and
`pg_get_wal_resource_managers()` is where to look. Once an index has been
written in rmgr mode the library must stay preloaded for as long as WAL that
mentions it may be replayed, which is the rule core states for every custom
resource manager.

`wal_mode` lives in the META PAGE, in `reserved[0]`, so LION_VERSION stays 6 and
a version-6 index written before this section has a zero there, which is exactly
"generic". The reloption `wal_mode` is `auto | generic | rmgr` with auto the
default: auto means rmgr when this server registered the manager and generic
when it did not, which is what lets one CREATE INDEX script work on a cluster
that preloads the library and on one that does not. Asking for `rmgr` by name on
a server without it is an ERROR with the preload hint, at CREATE INDEX (from
`lionoptions()`) and again at build. READING an rmgr-mode index works on any
server; WRITING one without the manager ERRORs in `lion_wal_begin()`, which is
the one place every write path passes through, with the preload hint and the
REINDEX alternative. REINDEX is what changes an index's mode.
`lion_index_wal_mode(idx)` reports it.

### Testing (implemented)

- `make installcheck` runs the whole suite in generic mode, which is what a
  server without the preload gives, and `make installcheck-rmgr` restarts the
  dev cluster with `shared_preload_libraries = 'pg_lion'` on pg_ctl's command
  line, runs the same suite, and restarts it back. Nothing is written into
  postgresql.conf, so an interrupted run leaves no trace. `WAL_CONSISTENCY=1`
  adds `wal_consistency_checking = 'pg_lion'`.
- `test/sql/walrecords.sql` drives every write path and reads the WAL back with
  pg_walinspect. It has TWO expected files, because the point is that the output
  differs: `walrecords.out` is the generic run and `walrecords_1.out` the rmgr
  one, and pg_regress accepts either. It uses pg_walinspect and not pg_waldump
  because pg_waldump does not load the module and therefore prints the manager
  as `custom128` and every record type as `UNKNOWN`; pg_walinspect runs in the
  backend, where `rm_identify()` and `rm_desc()` are ours.
  One trap, found the hard way and now closed in `test/rmgr-check.sh`: `pg_ctl
  restart` REUSES the options in postmaster.opts, so restarting after an `-o`
  start keeps the preload and leaves the dev cluster in rmgr mode - and the
  next plain `make installcheck` then silently tests the wrong mode and passes,
  because walrecords.sql's two expected files make either answer acceptable.
  Both directions stop and start instead, and the restore prints how many
  resource managers are registered afterwards.
- `test/recovery/run.sh` takes `--mode generic|rmgr` and a repeatable `--conf`.
  `make recovery-check-rmgr` is `--mode rmgr --conf "wal_consistency_checking =
  'pg_lion'"`, and THAT is where the "replay reproduces every page" proof
  actually lands: `wal_consistency_checking` only compares during REPLAY, so
  turning it on for a primary that never replays proves nothing. The harness's
  standby replays everything phases 1 and 2 write, and every crash of phase 1
  replays its own range; a mismatch is a FATAL in the startup process. Phase 3
  (the pinned standby reader, above) makes a standby of its own and replays
  VACUUM_VISIT and barrier-carrying records under the same check.

### Measured (2026-09-22, release build, alternating arms, one binary and one cluster)

The A/B lever is `shared_preload_libraries`: the benchmark cluster is restarted
with and without it, so `wal_mode = auto` resolves to rmgr or generic and
nothing else differs. Arms were alternated within every series. The box was NOT
idle (the regression suite ran throughout), so the millisecond columns carry
about +/-10% and the WAL columns - which were byte-identical across every
repeat - do not. Times are from a `-O2` build and are therefore NOT comparable
with the assert-build numbers elsewhere in this document; the WAL bytes are, and
the generic arm reproduces §5's "before" to within noise, which is the
cross-check that says so.

    8-client hot-key burst, 2 key values, synchronous_commit on
                      tps            p95
      generic         765            14.8 ms
      rmgr           1275             9.9 ms
      btree          1632             7.8 ms
      no index       2336-2371         -        (identical in both arms)

    ... and by client count (tps / p95)
                      1 client      4 clients     8 clients
      generic        456 / 2.9     702 / 8.7     761 / 15.0
      rmgr           515 / 2.7    1165 / 4.1    1313 /  9.5
      btree          522 / 2.6    1185 / 4.0    1600 /  8.2

**The hot-key ceiling of §5 is broken, and by more than §5 predicted was there
to break.** §5 measured an UNLOGGED control at 868 tps and concluded that the
8 KB page copy, not the logging, was the cost. rmgr reaches 1275-1313 tps -
PAST that unlogged ceiling, at 66-72% of btree where generic is 47% - which says
the copy was indeed the cost and that removing it also removed what the unlogged
control still paid (GenericXLog copies and diffs the page whether or not it
writes the record). At four clients rmgr is level with btree.

    INSERT 10k rows into the 1M-row, 8-index portfolio
                      after a checkpoint        steady state
      generic         922 ms / 88.10 MiB        853 ms / 53.53 MiB
      rmgr            254 ms / 64.53 MiB        142 ms / 29.21 MiB
      btree           169 ms / 33.31 MiB        104 ms /  6.75 MiB

    WAL per insert record (10,000 single-row inserts, one index, second batch)
      column   path        generic              rmgr
      c2       container   121.6 B  1.16 MiB    114 B ITEM_SET  1.62 MiB
      c20k     INLINE     2918.7 B 27.83 MiB    422.4 B ENTRY    4.03 MiB
      c1m      INLINE     1682.2 B 16.04 MiB    124.1 B ENTRY    1.18 MiB

    8-index portfolio build      generic 4077 ms / 57.47 MiB
                                 rmgr    4051 ms / 57.47 MiB
                                 btree   2653 ms / 59.25 MiB
    portfolio index size         generic 70,852,608 B = rmgr 70,852,608 B

**Two of those rows say something this section did not expect.**

*The build does not move at all, and cannot.* `wal_mode` has no effect on CREATE
INDEX: ambuild writes through the bulk-write API (§5 step 4), which logs whole
pages with `log_newpage`-style records and never touches the shim. 57.47 MiB in
both arms, to the byte. That is worth saying out loud so nobody expects build WAL
to move; lion's build already writes less WAL than btree's.

*The container path writes MORE WAL, not less: 1.16 -> 1.62 MiB per 10k inserts,
+40%.* The cause is visible in the record histogram: 9,631 of the 10,000 inserts
are a 114-byte ITEM_SET, which is the record this section was designed around and
is 8 bytes cheaper than the generic delta - but 361 of them take the general path
and emit an ITEM_REPLACE averaging **1658 bytes**, which is the whole rewritten
container item, where GenericXLog's byte diff of the same operation cost about
the same 122 bytes as everything else. §25 specified ITEM_REPLACE as "(block,
offset, newlen, bytes)" and that is exactly what was built; for a 1.6 KB
container it is the wrong trade. It costs no latency - the record is written
outside the lock window that matters and the burst numbers above are what they
are - but it is a real byte regression on dense keys, and the fix is known: an
item that is being rewritten at the SAME allotted length differs from its
predecessor in a handful of bytes, so ITEM_REPLACE should either carry a delta or
be split into a shrink/grow plus an ITEM_SET. **It was the first thing to do in
this area and it is done: LION_OP_DELTA above, and the re-measurement below.**

*The INLINE path is where the win is*, and it is the win §25 predicted for a
feature it did not build: 2918.7 -> 422.4 B and 1682.2 -> 124.1 B per insert, 7x
and 14x, purely from logging the rewritten entry tuple instead of a page diff
that covers the whole tail of the leaf. INLINE entry slack (the "also" item
below) would cut it again - and it does, in both arms, below.

Not measured, and honestly so: the quick-v3 `mid_insert` at 1M rows and the
VACUUM micro-benchmark. Both harnesses build their own cluster in ways that have
no hook for `shared_preload_libraries` (`bench/comprehensive/run.py` writes its
own postgresql.conf; `bench/vacuum_micro.sh` points at the dev cluster), so
neither can be run in rmgr mode without changing them. `mid_insert` is dominated
by the same INLINE entry rewrites that dropped 7x above, so it should fall well
below its 66 MiB, but that is a prediction and not a measurement.

*(One trap for whoever repeats this: `pg_ctl restart` re-uses the previous
postmaster's options out of postmaster.opts, so a plain restart after a preloaded
start stays preloaded and the "generic" arm silently is not one. Pass `-o ""`,
or stop and start. `test/rmgr-check.sh` had this bug and now stops and starts in
both directions and prints how many resource managers are registered
afterwards.)*

### Measured again after the delta and the INLINE slack (2026-09-22)

Same box, same release prefix, same 1M-row cluster and the same two arms; the
A/B swaps only the installed `pg_lion.so` (`base` is the commit above, `new` is
this one) and restarts the cluster with or without the preload.
`bench/write_micro.sh` grew `LION_WM_PGBIN` and `LION_WM_PRELOAD` so that this
is one command per cell. WAL bytes were byte-identical across repeats; the
millisecond columns are medians of 2-5 runs on a box that was not idle.

    WAL per insert record (10,000 single-row inserts, one index, second batch)
      column  path        generic  before -> after   rmgr  before -> after
      c2      container   121.6 B  -> 121.6 B        114.0 B ->  79.0 B  ITEM_SET
                                                    1657.8 B ->  98.4 B  ITEM_REPLACE
      c20k    INLINE     2918.7 B  -> 173.0 B        422.4 B -> 180.5 B  ENTRY
      c1m     INLINE     1682.2 B  -> 106.8 B        124.1 B ->  82.0 B  ENTRY

    ... per 10k-row batch, the same thing in MiB of index WAL
      c2       generic 1.16 -> 1.16     rmgr 1.62 -> 0.76
      c20k     generic 27.83 -> 1.65    rmgr 4.03 -> 1.72
      c1m      generic 16.04 -> 1.02    rmgr 1.18 -> 0.78
      (btree on c200, for scale: 0.63 MiB, 64 B per leaf insert)

    INSERT 10k rows into the 1M-row, 8-index portfolio (before -> after)
                    after a checkpoint            steady state
      generic    893 -> 935 ms  88.10 -> 90.82   851 -> 768 ms  53.53 -> 9.48 MiB
      rmgr       285 -> 289 ms  64.53 -> 51.68   143 -> 130 ms  29.21 ->  9.08 MiB
      btree      150 ms         33.31            93 ms           6.75 MiB
    index size after 20k inserts: 70,967,296 B in BOTH arms, to the byte

    8-client hot-key burst, 2 key values, synchronous_commit on, rmgr mode
                       tps          p95        WAL/row   of which the index
      before          1328         9.31 ms      238.8 B      166 B
      after           1290         9.64 ms      159.0 B       86 B
      btree           1642/1644    7.9  ms      143.1 B       70 B
      no index        2394/2461    3.9  ms       72.6 B        -
    (the index share is the row minus the no-index control, which is the same
    heap in every arm).  The btree and no-index arms moved under 1% between the
    two runs, so the 3% the lion arm lost is probably real: it is what the delta
    comparison costs inside the critical section, against half the index WAL.

**The +40% is gone and the container path is now cheaper than generic WAL.**
1.62 -> 0.76 MiB against generic's 1.16: the 361 general-path rewrites fell from
1658 to 98 bytes, which is 16.8x, and the 9,631 hot-key records fell from 114 to
79 because the entry tuple that travels with every one of them is now a delta of
its `ntids` counter instead of a copy of the whole 40-byte tuple. That second
number is the one to remember about DELTA: it pays on the SMALL records too.

**The INLINE slack pays most where the rmgr could not reach**: generic mode,
where the record is a diff of the page and the old code moved every entry after
the one it grew. 2918.7 -> 173.0 B on c20k is 16.9x and brings generic-mode
INLINE inserts within 2.7x of btree's leaf insert. In rmgr mode the same change
is 2.3x (422.4 -> 180.5), because the record was already the entry alone; what
is left is a sparse segment's own shape - inserting a pair shifts both the
`ckeys[]` and the `los[]` array inside the payload, so the delta covers the tail
of both. An operation that replayed `lion_sparse_insert()` against an unaligned
payload (redo would have to copy it out, call the library function and copy it
back) would cut that to a dozen bytes, and it is the obvious next step; it is
not done here. It is also why generic (173.0 B) is now marginally CHEAPER than
rmgr (180.5 B) on c20k: a byte diff of a page describes that shift in fewer
bytes than four-byte range headers do.

*Two costs, both small and both real.* The steady-state portfolio insert is
unchanged to slightly faster in rmgr mode (143 -> 130 ms) - the delta comparison
runs inside the critical section, so it is written to skip equal bytes eight at
a time, and the page-tail memmove it removes is worth more than it costs - but
the first batch after a CHECKPOINT writes 2.7 MiB MORE in generic mode
(88.10 -> 90.82), which is full-page images: entries that carry slack fill a leaf
a little sooner, so a batch touches a few more pages the first time round. The
index itself is the same size, to the byte, after the same inserts.

Also in this wave (§23 items that need the same lock-window work):

- **INLINE entry slack: DONE**, in the wave after this one, and §4 carries it
  (see "Growth slack inside an INLINE payload"). It took the shape this section
  predicted - the zero-terminator convention of §18, no header field, no use of
  §24's two spare bytes - and one thing it did not: the record is not an
  ITEM_SET with a SETBYTES, it is the ordinary ENTRY record with the DELTA
  operation above, because that operation had to exist anyway for ITEM_REPLACE
  and it describes this change exactly (the counters in the header, and the
  payload from the insertion point up). `lion_insert_inline()` still rebuilds
  the payload in a work buffer and still decides everything before the record
  opens; what changed is the LENGTH it writes the entry at. When the new payload
  fits the bytes the entry already has, the allocated length does not change, so
  PageIndexTupleOverwrite() moves no other entry on the leaf - which is where
  the generic-mode 16.9x comes from - and when it does not, the entry is written
  with fresh slack, bounded by inline_limit, by the page's free space and by
  LION_ENTRY_SLACK_BOUND.
- **Early exit in the AND merge: DONE** (`lion_run_merge()` in lion_count.c).
  The non-driver sources are ordered by ascending members - the same `ntids` §22
  picks the driver from - and the merge seeks and folds them in that order,
  abandoning a container key the moment the running intersection is empty or a
  seek lands past the target. The sources that were not sought are not stepped
  and keep their pins.
  **§9 is untouched, and here is why**: an abandoned container key contributes
  nothing to the answer, so no visibility-map question is asked about it, so
  there is no obligation to discharge for any page it touched; a pin too many
  never makes a count wrong, it only makes VACUUM wait. The pages of sources
  that WERE sought are released by `lion_ecursor_seek()`, the same window at the
  same kind of key as before.
  A `Probes Avoided` counter is in `LionCountStats` and in EXPLAIN ANALYZE, and
  `test/sql/pushdown.sql` pins three shapes: one where the two selective sets
  are provably disjoint (200 divides 20000) and the third source is never
  sought, and one where one set provably contains the other (20 divides 200) and
  nothing may be skipped, before and after the heap is dirtied.
  Measured at 1M rows: `c20k = 77 AND c200 = 17 AND c2 = 1` goes from **112 to
  11 buffer accesses** and 104 to 70 containers, 0.095 to 0.040 ms; a four-source
  AND goes from 127 to 14 buffers. Two-source ANDs and every GROUP BY are
  unmoved, because there is no third source to skip - §22's "118 buffers probed
  for c2" is the two-source shape and was never going to move. The all-dense
  three-source case reads 4% MORE buffers (262 -> 273) while visiting 80 fewer
  containers, which is `LION_POSTING_SEEK_STEPS = 2` biting: a source left
  standing falls further behind and its next seek pays a descent where it used
  to step one leaf right. It is a wash in wall time and the threshold was not
  tuned for it.
  **The §22 cost formula was deliberately NOT changed**, and the measurement is
  the reason. The candidate - multiply `probes` by an expected survival fraction
  per level - predicts the new behaviour accurately when the columns are
  independent and is 10x optimistic when they are not, and the planner cannot
  tell the two apart from the inputs `lion_cost_count_rel()` has. The decisive
  pair, one table, two queries with IDENTICAL marginal statistics and the same
  estimate of 245.25: `k1 = 17 AND k2 = 17 AND d = 1` where k1 and k2 are the
  same column avoids **0** probes and reads 236 buffers, while `k1 = 17 AND
  k2 = 18` avoids **223** and reads 13. A survival fraction built from the
  marginals charges ~24 probes for both; for the disjoint query that is right
  and for the correlated one it is ten times light. So `pages = Min(walk,
  probes x (height + 1))` stays as it is: after this section it is an UPPER
  BOUND the executor can only beat, and it is still exact for the second source,
  which is probed at every driver key. If a discount is ever wanted, the
  defensible form is to take the survival fraction from
  `clauselist_selectivity()` over the conjunction of the already-folded clauses
  rather than from a product of marginals - that is the one number in the
  planner that consults extended statistics, so a user who declares the
  correlation gets the full charge and one who does not gets the independence
  assumption the rest of the planner already runs on. Keep the `Min(walk, ...)`
  cap either way, and re-check every pin in `test/sql/pushdown.sql` (§10's
  20000-group refusal and §20's 200x20 refusal are the ones to watch), because
  it moves every AND estimate.

### Measured: what the standby barrier costs (2026-09-22)

`bench/vacuum_micro.sh --only portfolio` (1M rows, eight indexes, `DELETE ...
id % 100 = 1`, one VACUUM) on a private cluster of the assert build, alternating
arms of the binary before the barrier (HEAD 453fa32) and after, same data
directory, same settings:

                           VACUUM ms        index WAL        index records
    rmgr     before         686, 678        51 MB (50 FPI)       10,245
    rmgr     after          688, 649        51 MB (50 FPI)       10,245
    generic  before         780             59 MB (50 FPI)       10,245
    generic  after          775             59 MB (50 FPI)       10,245

No record is added: at a 1% delete nearly every page VACUUM visits is one it
changes, and the 1,420 visited-but-unchanged blocks ride on 1,255 of the
existing removal records as 10,128 bytes of ranges - 0.02% of the index WAL -
with no stand-alone VACUUM_VISIT record at all. A sparse VACUUM is the
unfavourable case: `DELETE ... id % 10000 = 8` (100 rows) then `VACUUM
(INDEX_CLEANUP ON)` wrote 1,046 VACUUM_PAGE records, 749 of which carried a
barrier of 7,488 blocks in 11,960 bytes, 0.15% of its 7.8 MB of index WAL, and
still no stand-alone record. The generic arm writes no barrier (a generic-mode
standby rechecks every TID, §9) and pays only for the cleanup locks of the
posting-tree descents, which are not measurable here. Replay pays one buffer
lookup and one uncontended cleanup lock per barrier block, and only in hot
standby.

## 26. `count(DISTINCT k)` in the pushdown (v1, implemented)

Formerly the §23 backlog item of the same name. Two shapes, with `k` - and `g` - scalar key columns
of lion indexes (any column of a multicolumn one, §24):

1. `SELECT count(DISTINCT k) FROM t [WHERE <pushdown quals>]`
2. `SELECT g, count(DISTINCT k) FROM t [WHERE <pushdown quals>] GROUP BY g`

**What is counted.** A scalar index puts every row under exactly one entry of `k` - its value's, or
the reserved NULL entry (§14, and §15 for the argument) - so the distinct non-NULL values of `k`
among the rows the WHERE selects are exactly the non-NULL entries of `k` whose posting set,
intersected with the WHERE sources (and, in shape 2, with the group's set), holds at least one row
visible to the snapshot. The NULL entry is never counted (`count(DISTINCT k)` ignores NULLs), and
the reserved EMPTY entry of §17 cannot occur because multi-key columns are refused. Shape 1 is
therefore §10's GROUP BY-`k` walk with each group's count replaced by an EXISTENCE test and the
groups summed into one row; shape 2 is §20's nested loop over (outer `g`, inner `k`) with each
pair's count replaced by an existence test and the inner loop summed into one row per outer group.

### The existence test (`lion_exists_sources_cached()`, lion_count.c)

The same merge as a count - the same sources, the same set algebra, the same visibility-map check
per container, the same recheck batch and visibility cache - with one difference: it stops as soon
as the answer is known.

- After every container the merge has put through the visibility map (the VM check itself,
  `lion_count_container_vm()`, is unchanged) the test asks whether anything has been counted. A
  member on an all-visible block settles it at once, and the recheck TIDs the container queued, if
  any, are dropped unread.
- Otherwise the container's dirty-block TIDs are rechecked right there - the ordinary
  `lion_recheck_flush()`, called at a container boundary, which is a heap-block boundary - and one
  visible row settles the test.
- Only when the sets run out is the answer no. A group whose rows are all on dirty pages therefore
  rechecks container by container until its first visible row and never more than one container
  past it; a group on an all-visible heap costs the first container of its intersection and
  nothing else.

The disjoint sum of §15 and the single-set path stop the same way, between two sets and between
two containers respectively. The heap recheck and the visibility cache are called, not changed.

**Why §9 and §11 cover existence exactly as they cover counting.** An existence test is a count
compared with zero, computed from the same containers read the same way; the only change is that
the count stops growing early, and stopping reads less, never differently. Every "yes" is one of
the two things a count adds a row for: a member of a container on a block the visibility map
reported all-visible WHILE the page that container was copied from was still pinned - the rule of
§9, which the unchanged `lion_count_container_vm()` enforces: VACUUM cannot have finished
ambulkdelete on that page, so it cannot have set all-visible on a block holding a dead TID of that
container - or a TID the heap recheck found visible under the snapshot, which needs no pin at all
(§9 step 5). Every "no" is a count of zero. Dropping queued recheck TIDs after a VM hit discards
candidates that could only have added to an answer already known to be positive. So the §11 VACUUM
rule needs no addition. `test/isolation/count_distinct_vacuum_race.spec` proves it the way
`count_vacuum_race.spec` proves it for a count: the key whose rows are all dead comes first in the
entry walk, the distinct count parks at the existing `lion-count-containers-pinned` injection point
with that key's container pinned, the VACUUM that would remove those rows and mark their pages
all-visible is seen waiting for the pin, and the answer - for both shapes - excludes the dead key
and includes the key deleted after the snapshot.

### Executor

- Shape 1 (`lion_distinct_relation()`): walk `k`'s entries - or, when the WHERE pins `k` itself with
  `k = c` or `k = ANY (list)`, that clause's located sets, exactly as §15's GROUP BY driver does
  (the clause is then not intersected: an entry intersected with a union of disjoint entries that
  contains it is the entry), so `k = c` answers 0 or 1 in one test. A `k IS NOT NULL` clause on
  the driving column is dropped and the NULL entry skipped, as for §14's sum-over-all. One test
  per non-NULL entry, one row out.
- Shape 2 (`lion_next_group_distinct()`): for each outer `g` entry, first the group itself - does
  `g ∩ WHERE` hold a visible row? A group with none is not emitted, and a group whose rows all have
  `k` NULL is emitted with `count(DISTINCT k) = 0`, as SQL requires. Then one test per non-NULL
  inner key: the inner keys are read once per relation and each inner set is located per pair,
  which is §20's pin budget (two group pins at a time). One row per group. The outer set and the
  WHERE sets are the ones every pair reuses, so §9's materialization applies to them as in §20.
  An IN list on `g` does not drive the groups here (the entry scan does), which keeps the output
  in `g`'s directory order and the pathkeys of §21 valid.
- Mixed target lists. `count(*)` and `count(col)` (under §14's rules) may stand beside
  `count(DISTINCT k)`, and so may `count(k)` - the number of non-NULL `k` - which a nullable `k`
  would not otherwise allow. Whether a test must COUNT rather than stop early is decided from the
  target list's kinds: in shape 1 any other count makes every entry test a full count (the total
  is the sum over all entries, the NULL entry included - §15's disjointness again - and `count(k)`
  the sum over the non-NULL ones); in shape 2, `count(*)` and `count(g)` make the GROUP test a
  count and `count(k)` makes the PAIR tests counts. A pure distinct count is existence tests only.
- A GROUP BY the planner folded to constants (`WHERE g = 3 GROUP BY g`) is shape 1 under §10's
  single-group rule - no row when nothing matches. Whether a row matches when every matching row
  has `k` NULL is one more existence test, on `k`'s NULL entry, made only when no distinct value
  was found.
- HAVING on the distinct count is the plan.qual of dff300f unchanged: the distinct count is a
  column of the node's scan tuple like any other count.

### Planner acceptance (`lion_try_count_path()`)

- Each `count(DISTINCT x)` must be `count(any)` of one argument that is a plain column of the
  relation (relabels stripped), with no FILTER and no ORDER BY, not variadic and not split, and
  every distinct aggregate of the target list and the HAVING must name the same column with the
  same equality and collation. `count(DISTINCT (a, b))`, `count(DISTINCT a + 1)`, other DISTINCT
  aggregates and two different distinct columns decline.
- `k` must not be a GROUP BY column, and there may be at most one GROUP BY column (so at most two
  driving columns, `LION_MAX_GROUPCOLS`).
- `k` goes through `lion_collect_targets()` as a driving column, so every per-column rule of §10
  applies to it unchanged: a SCALAR lion index (a multi-key one is never found, so
  `count(DISTINCT tags)` - which counts distinct ARRAYS, not elements - declines); the index's
  collation must be the DISTINCT's (`exprCollation()` of the argument, which is what nodeAgg sorts
  and compares with, so `count(DISTINCT t COLLATE "C")` over a default-collation index declines);
  and strategy 1 of the index's opfamily must be the DISTINCT's own equality operator
  (`SortGroupClause.eqop` of `aggdistinct`, compared by Oid - the grouping-equality check of the
  2026-09-20 review, finding 3), so a coarser opclass declines and `citext_ops` over citext is
  accepted. No value of `k` is printed, so the representation gate of finding 4 does not apply.
- Partitioned tables decline: distinct counts are not additive across partitions, and §16's shape
  (partial aggregates added up by a Finalize Agg) would add them. A later version could emit
  per-partition (g, k) pairs for core to deduplicate.
- Everything else - WHERE clause kinds, OR, IN lists, Params, HAVING, privileges, RLS - is §10 as it
  stands. A query with no WHERE clause at all is accepted: `k`'s entries drive it.

`custom_private` gains `LION_PRIV_DISTINCT` (IntList: the distinct column's attnum, or empty) in
front of the target-list kinds - shape 8, `LION_PRIV_NMEMBERS` 11 - and the target-list kinds gain
`LION_TL_COUNT_DISTINCT` and `LION_TL_COUNT_DISTCOL` (`count(k)`). `k`'s index travels in the
existing Oid slots, the driving one in shape 1 and the inner one in shape 2; `groupattno2` stays 0,
because `k` is not a grouping column, and the executor calls the inner column `innerattno`.

### Cost

`lion_cost_count_rel()` prices shape 1 as §10 prices a GROUP BY over `k` - one entry scan, and the
per-group container work for every entry, because every entry is tested whether or not the WHERE
leaves it a row (the entries are `n_distinct(k)` over the whole table) - and shape 2 exactly as §20
prices the (g, k) nested loop: one `cpu_tuple_cost` per PAIR, the intersection term
`Min(|G|, |K|) x rows` and both sides' container bookkeeping per pair. That is what charges pairs
and not groups, and what makes a large `|G| x |K|` lose to the sorting aggregate core builds for a
distinct count - the decline the backlog item asked for, with the cost model as the guard, as in
§20. On top of that:

- **A fixed cost per test**, `LION_DISTINCT_TEST_COST` (50 x `cpu_tuple_cost`), for every entry
  test, group test and pair test: a merge set up and torn down, and for a pair the inner set's
  lookup. Measured on the assert build at 100k rows, a test costs about 2 us with nothing to
  intersect and 3.4 us with a WHERE set to seek, against about 0.2 us per row for the sort-based
  aggregate, whose model charges about 0.1 per row: a test is worth some ten of its rows. Without
  it, a walk of 1000 entries of `k` under a WHERE that leaves 500 rows was chosen, and took 3.4 ms
  against the bitmap scan and sort's 0.56.
- **The early exit is discounted only where it is real.** In shape 1 each entry test reads the
  share of its intersection `lion_exists_fraction()` expects: a test whose intersection is expected
  to hold `s >= 1` rows over `D` containers stops after about `D/s + 1` of them, and one expected to
  be empty (`s < 1`) reads everything, as a count would. The recheck candidates are scaled the
  same way, in both shapes, because a test rechecks one container at a time and stops with them.
  The pair terms of shape 2 are NOT discounted: a pair's time is its lookup and its merge setup
  far more than its members - measured per pair, an existence test took 6.5 us against 7.7 for
  §20's count of the same 200 x 50 pairs - and discounting them chose 200 x 50 pairs at 64.6 ms
  against the sort's 43.
- Tests that must COUNT (the mixed target lists above) are charged in full.

Measured with the final model (100k rows, uncorrelated columns, warm cache, assert build - ratios,
not absolute numbers; `test/sql/distinct.sql` pins the rows marked *):

| query | LionCount | core plan | chosen |
|---|---|---|---|
| `count(DISTINCT k50)` * | 0.68 ms | 18.4 ms | node |
| `count(DISTINCT k1k)` | 2.04 | 15.7 | node |
| `count(DISTINCT g2k)` | 3.26 | 16.0 | node |
| `count(DISTINCT k50) WHERE g8 = 3` | 0.90 | 2.69 | node |
| `count(DISTINCT k1k) WHERE g200 = 3` | 3.16 | 0.39 | core |
| `g8, count(DISTINCT k50)` * | 5.41 | 51.9 | node |
| `g8, count(DISTINCT k1k)` | 35.8 | 60.0 | node |
| `g200, count(DISTINCT k50)` * | 57.5 | 37.8 | core |
| `g2k, count(DISTINCT k50)` | 401 | 40.8 | core |
| `g200, count(DISTINCT k1k)` | 693 | 44.1 | core |
| `g2k, count(DISTINCT k1k)` * | 5699 | 39.2 | core |

Every choice in the table is the faster plan. The open item is the one §20 records: the model does
not model correlation, so clustered data, where most pairs' merges end at once, is priced as if it
were not.

### EXPLAIN

`Lion Indexes` names `k`'s index like a group index - `ix_k (k)` first in shape 1, after the outer
group's index in shape 2 - then the WHERE indexes; `Group Key` is `g` alone; a new line
`Distinct Key: k` says which column is counted. With ANALYZE, `Distinct Keys Tested` counts the
entry and pair tests made.

### Tests

`test/sql/distinct.sql`: every query against a forced sequential scan with the pushdown off, as a
multiset both ways round - NULLs in `k` and in `g`, deletes on dirty pages and after VACUUM, IN
lists on `k` itself and on another column, `k = c`, `k IS NOT NULL`, an OR, Params under a generic
plan, the mixed target lists, HAVING, a folded GROUP BY, citext, a multicolumn index (shape 2 out
of one index) - and the declines: an array column, a partitioned table, a collation mismatch, a
coarser opclass, an unindexed column, two distinct columns, `k` as the group column and two GROUP
BY columns. The plan choice for shape 2 is pinned at a small and a large pair count, and EXPLAIN
ANALYZE shows the early exit reading fewer containers than the same walk with `count(k)` beside it.

## 27. FK-side join pushdown: `GROUP BY dim.attr` over a fact table joined on a lion-indexed FK (v1, implemented)

Formerly the §23 backlog item of the same name. The shape is the star-schema aggregate

    SELECT d.attr, count(*)
    FROM fact f JOIN dim d ON f.fk = d.pk
    [WHERE <pushdown clauses on f>] [AND <anything on d>]
    GROUP BY d.attr [HAVING ...]

and its ungrouped form, `SELECT count(*) FROM fact f JOIN dim d ON f.fk = d.pk WHERE d.attr = c`,
a semi-join-like count over the fk keys of the matching dimension rows. The ordinary plan reads
every fact row the fact filters leave, probes the dimension for each and aggregates; the posting
sets already hold the fact rows of every fk value, partitioned by value.

### What is computed, and why it is exact

Let `D` be the dimension rows that survive the dimension-side quals under the query's snapshot, and
for one of them, `d`, let `F(d)` be the fact rows that satisfy the fact filters and `f.fk = d.pk`
under the join's operator. The inner join's `count(*)` for a group `G` of `d.attr` is the number of
join PAIRS `(f, d)` with `d` in `G`, which is `sum over d in D ∩ G of |F(d)|`. So the node never
builds the per-group key set the backlog item sketched:

- it runs the dimension side as an ordinary child plan (below) and, for EACH dimension row, looks
  `d.pk` up in the fact's lion index on `fk` and counts that one posting set ANDed with the fact
  filters - the §9 count of `f.fk = <value> AND <fact filters>`, with the value taken from the
  dimension row instead of from the query;
- it emits one PARTIAL aggregate per dimension row whose count is not zero - `(the dimension
  columns the query needs, partial count)` - and core's Finalize Agg above it groups those rows by
  `d.attr` with `d.attr`'s own equality and adds the counts, exactly as §16's partitioned GROUP BY
  hands its per-partition partials to it.

Consequences, each of them a rule rather than an optimisation:

- **No per-group union, so no memory bound to enforce on one.** The node holds one dimension row,
  one fk posting set (located, counted and released per row, like a §20 inner pair's) and the fact
  filters' sets for the whole scan. The grouping state lives in core's Finalize HashAggregate,
  which spills under `hash_mem` (§16's argument, unchanged). A dimension of a million rows is not
  refused for memory; it is refused by the cost model, because a million lookups lose to a hash
  join.
- **Grouping equality and value representation are core's.** `d.attr` is never read from an index:
  its value comes out of the dimension's own tuples through the child plan, and the Finalize Agg
  groups it with the `SortGroupClause` the parser chose. Neither §10 finding 3 (the driving index's
  equality is the grouping equality) nor finding 4 (the value-representation contract) applies,
  because no index key is printed and no index drives the groups. Any grouping over dimension
  columns - `GROUP BY d.a, d.b`, `GROUP BY upper(d.name)` - is therefore fine: the node's
  projection computes it per dimension row from the child's columns. A VOLATILE one is refused,
  since evaluated per dimension row instead of per join row it would mean something else.
- **HAVING belongs to the Finalize Agg**, as for a partitioned GROUP BY (§16): any HAVING core can
  evaluate over the grouped target is accepted, as long as every aggregate in it passes the count
  test below.
- **NULLs.** A NULL `d.pk` joins nothing (the join operator must be strict) and is skipped without
  a lookup; a NULL `f.fk` lives in the fk index's reserved NULL entry (§14), which no lookup of a
  value reaches. A NULL `d.attr` is a group like any other, formed by the Finalize Agg.
- **A dimension row with no fact rows emits nothing**, so an inner join's absent group stays absent;
  the ungrouped form's plain Finalize Agg then answers 0 for an empty input, as `count(*)` over an
  empty join does. A GROUP BY the planner folded to constants (`WHERE d.attr = 3 GROUP BY d.attr`)
  gets a zero-column sorted Finalize Agg, as core builds itself, so an empty join has no row.

### Uniqueness of the dimension key

The sum above is the join count whatever `d.pk` holds - a duplicated key is two dimension rows,
each counted with its own multiplicity, which is exactly what the join does - so, unlike the
backlog sketch's union (where two rows of one group with one key would have been counted once),
this design does not need uniqueness to be RIGHT. v1 requires it anyway, as a scope decision:

- the ordinary star schema has it (the dimension's primary key), and it bounds the node's work by
  the dimension's KEY count, which is what the cost model assumes: every lookup finds a different
  fk entry, so no fact row is read twice;
- a non-unique key is the shape where a hash join over the fact wins in any case, because the
  lookups multiply, and it is kept out of the tested surface.

The proof is `lion_fkjoin_dim_unique()`: a single-column, UNIQUE, immediately enforced, non-partial
btree index on the dimension's join column whose opfamily contains the join operator as its
equality strategy and whose collation equals the join clause's input collation (or the type is not
collatable). It is written against the index list rather than through core's
`relation_has_unique_index_for()`, because before PostgreSQL 19 that function does not compare
collations (its own `XXX`): a unique index under `"C"` does not make a join under a
case-insensitive collation unique. Lifting the rule is a costing change, not a correctness one.

The proof is therefore a COSTING AND SCOPE GUARD ONLY, and nothing about correctness rests on it
(2026-09-23 review): because the per-dimension-row sum is exact with duplicates, a unique index that
is later dropped, becomes invalid, or is deferrable and violated inside the current transaction can
make the node do more lookups than it was priced for, never give a wrong answer.

### The join semantics the lookup must reproduce

The count for one dimension row is the §10 count of `f.fk = v` with `v = d.pk` of that row, and a
lookup answers that clause exactly when the clause is one the pushdown already accepts:

- the join operator is strategy 1 of the fk index's opfamily, the compared type (`d.pk`'s) has a
  cross-type equality and a hash function in that family, and the collations agree - exactly
  `lion_match_index()` for a `LION_CLAUSE_EQ` clause, which the join key is fed through, per
  relation. Cross-type keys (`int4` fk against `int8` pk) work wherever `int4col = 8::int8` works,
  and an opclass on `fk` whose equality is not the join operator never matches;
- the operator is strict, so a NULL key joins nothing;
- the join is the ONLY join clause: one equality between a plain column of the fact rel and a plain
  column of the dimension rel, from an equivalence class or from `joininfo`. A second join clause
  (`AND f.a < d.b`, a composite key), a pseudoconstant qual anywhere in the query, an outer, semi or
  anti join (`join_info_list` must be empty), a LATERAL reference or a PlaceHolderVar all decline.
  An equivalence class with a constant (`d.pk = 5`) generates no join clause at all - both sides
  are restricted to the constant - and is left to the ordinary plan.

### Visibility and the §9 interlock

- **The dimension side is an ordinary plan**: the dimension rel's cheapest total path, planned by
  core with its restriction clauses and its RLS security quals (with core's leakproofness rules),
  carried as the CustomPath's `custom_paths` child and run by the node under the query's snapshot.
  Only rows that passed it are ever looked up. Privileges on both tables are the executor's
  range-table check, as before; EXECUTE on the join operator, which the node evaluates in place of
  a hash or nested-loop join, is the node's own check at executor startup (§9, "Privileges"). The
  fact side keeps §10's rule: a fact rel with security quals
  (RLS) or TABLESAMPLE declines, and so does a partitioned fact table in v1.
- **The fact side is the §9 count, unchanged.** Each dimension row's fk set is located with
  `lion_posting_set_lookup_col()`, counted with `lion_count_sources_cached()` beside the fact
  filters' sources, and released before the next dimension row, so at any moment one fk set and
  the WHERE sets hold pins. The pin budget of §15 applies to that lookup as to every single lookup
  (a NOPIN set is re-located, or covered by another source, by the count itself), and no argument
  of §9, §11 or §15 changes: the fk set is to this node what one group's set is to §15's GROUP BY
  driver. The visibility cache is shared across dimension rows as it is across groups, and the fk
  index takes the `PredicateLockRelation()` every index the node opens takes.
- **One snapshot for both sides.** The child and the count both read `es_snapshot`, so a fact row
  is counted for a dimension row exactly when both are visible to it - which is the join.
- **The WHERE sets' pins are held across the child.** The fact filters are located once per scan
  and keep their pins (§9) while the node calls `ExecProcNode()` on the dimension child between
  counts. A child that runs user code - a slow function in a dimension qual, a `pg_sleep()` - can
  therefore keep a manual VACUUM of the fact table waiting on its cleanup lock on those index pages
  for the whole query (2026-09-23 review). That is the exposure every scan that holds a pin while
  the query runs has - a core index scan parked under a slow qual, or §10's streaming GROUP BY
  node, whose WHERE pins live from its first group to its last - and not a correctness issue: the
  VACUUM waits (autovacuum skips the page), and the query ends.

### Planner integration

`create_upper_paths_hook` at UPPERREL_GROUP_AGG, as today, with the input rel a JOIN rel of exactly
two plain base tables. `lion_fkjoin_recognize()` (src/lion_fkjoin.c) finds the one join clause and
proves the dimension key unique, for each orientation in turn, and hands `lion_try_count_path()` a
`LionFkJoin`. That function then runs over the FACT rel, unchanged for everything about the fact
side - the WHERE analysis of §10/§14/§15/§17/§19, Params, index matching per clause - with the join
key appended as one more `LION_CLAUSE_EQ` clause whose value expression is the dimension's Var, so
that `lion_collect_targets()` finds and checks the fk index for it like any clause. It differs only
in what it lets through the target list:

- the grouped target and the HAVING may reference only dimension Vars (in any non-volatile
  expression) and count aggregates: `count(*)`, and `count(x)` where `x` is a non-NULL constant or
  either side of the join key (non-NULL in every joined row). Other aggregates, DISTINCT, window
  functions, SRFs, grouping sets and row marks decline, as for a single table;
- the aggregates must be splittable (`GROUPING_CAN_PARTIAL_AGG`) and every grouping column
  hashable. The node's target is `lion_make_partial_target()` of the grouped target, and what goes
  into the grouped rel is `create_agg_path(AGG_HASHED, or AGG_SORTED over zero columns for a folded
  GROUP BY, or AGG_PLAIN; AGGSPLIT_FINAL_DESERIAL)` over the node.

Executor encoding: the join key clause travels in the clause lists like any other - its value, the
dimension Var, lands in `custom_exprs`, where setrefs.c rewrites it into an INDEX_VAR reference to
`custom_scan_tlist`, which is how EXPLAIN deparses it as `fk = d.pk` - but it is not a source and
is never located with the WHERE clauses. A new member `LION_PRIV_JOIN` names it and the child-plan
column that carries its value; the node reads the key straight from the child's tuple, and the fk
set occupies source slot 0 (the group slot, unused without an index-driven GROUP BY).
`custom_scan_tlist` holds every dimension Var the target needs, the key's, and the partial counts;
a dimension column's target-list kind is its position in the child's target list. Shape marker 9,
`LION_PRIV_NMEMBERS` 12.

**Cost** (`lion_cost_fkjoin_rel()`): the child's total cost, plus per dimension row one directory
descent (the leaf at `lion_heap_page_cost()`'s interpolated cost, capped by the directory's pages:
dimension rows come in heap order, not key order), a fixed per-count cost for a merge set up and
torn down (`LION_FKJOIN_COUNT_COST`, 50 `cpu_tuple_cost`, the order §26 measured per test), the fk
set's containers - `lion_containers_for(heap_pages, rows per fk value)` - at §10's two
`cpu_operator_cost` each, a PROBE into each fact filter's set at the container keys the two have in
common (`LION_FKJOIN_PROBE_COST`, 30 `cpu_operator_cost`, measured below), the set's chain pages
(none when its entries are INLINE), and a `cpu_tuple_cost` per emitted row; each fact filter's
lookup and chain once (located once, materialized on second use, §9); and §10's recheck term for
the fact rows the dimension rows reach - `min(D × rows per fk value, fact rows) × filter
selectivity × dirtyfrac` candidates, on dirty pages fetched once per query. Nothing charges the
whole fact heap: that is what the node exists not to read. The Finalize Agg is costed by core.

The probe term is the one that decides against the node, and it had to be measured: a probe (a
seek of the materialized filter set and the AND of two containers) costs about ten times what §10
charges a container. Priced like a container, the node was chosen for `... WHERE f.x = 3 GROUP BY
d.attr` below at 157 ms against the ordinary plan's 63; at 20 `cpu_operator_cost` it still was
(23,378 against 28,057); at 30 it is refused, while the same query over a quarter of the
dimension (39 ms against 67) is still chosen.

**A fact filter that is a UNION** - an IN list, or an OR across columns, whose leaves and their
lists' elements are the union's sets - is priced per SET, per count (2026-09-23 review). It is
located once, but every count builds its k-way union again (§15): each sub-cursor is set up and
positioned whether or not the fk set's keys find anything in it, and the containers standing at the
probed keys are merged. So every count pays `LION_FKJOIN_SET_COST` (80 `cpu_tuple_cost`) per set,
measured - `t IN (n values)` over 300 dimension rows of a 1.5M-row fact took 57 ms at n = 30, 199
at 100, 599 at 300 and 1195 at 1000, 4 to 6 us per set per count - plus `lion_merge_ops()` of the
whole source prorated to the share of the heap's container keys its probes touch. Priced as one
set, the review's `WHERE f.t IN (1000 texts) AND d.pk < 300` was chosen at 1.2 s against the nested
loop's 2 ms, and forced with `d.pk < 2000` estimated 7,500 against 10,000 and ran 8.1 s against
38 ms; it is now refused (326,000 against 10,180), and so are the dense-list shapes where the
per-count union is worst (`x IN (1, 2)` over 1000 dimension rows: 957 ms, refused, against 108).
The rows of the table below are unchanged by it.

An IN list whose array is a PARAMETER (`= ANY ($1)`) is refused outright: its length is not known
until the executor has it (§15), and the node's work per dimension row is proportional to it - a
43,000-value array over 20,000 dimension rows ran for minutes in the review. A single table
answers such a list once and keeps accepting it; `IN ($1, $2)` keeps its length at plan time and
is priced like a literal list.

**Measured** (2026-09-23, prune slot's PostgreSQL 20devel install - assert-enabled, so ratios and
not absolute numbers; two million fact rows over 22,728 heap pages with a 40-byte pad, `fk` = a
hash of the row number over 1..1000, `x` 10 values; a 1000-row dimension with 20 `attr` groups and
a quarter of its rows `region = 'eu'`; lion indexes on `fk` and `x`; warm cache, best of five,
`max_parallel_workers_per_gather = 0`). "Ordinary" is the plan the planner makes with the pushdown
off - on 20devel a hash join over a sequential scan, or with `x = 3` a partial aggregate below the
join over a bitmap heap scan:

| query | all-visible heap: node | ordinary | every page dirty (5% of rows updated): node | ordinary |
|---|---|---|---|---|
| `d.attr, count(*) ... GROUP BY d.attr` | **53.8 ms** | 339 | **272** | 350 |
| `... WHERE f.x = 3 GROUP BY d.attr` | 157 (refused) | **66** | refused | **73** |
| `... WHERE f.x = 3 AND d.region = 'eu' GROUP BY d.attr` | **39.8** | 68 | refused | **75** |
| `count(*) ... WHERE d.attr = 5` (50 dimension rows) | **2.7** | 176 | **101** | 193 |

Bold is the plan the cost model picks. The dirty column is the recheck of §9 at work: every heap
page holds an updated row, so every candidate TID is resolved against the snapshot, once per page
per query through the visibility cache.

**Parallelism**: `parallel_safe = false` like the rest; the child may itself be a Gather, which is
fine below a non-parallel node.

**EXPLAIN**:

    Finalize HashAggregate
      Group Key: d.attr
      ->  Custom Scan (LionCount)
            Lion Indexes: fact_fk_idx (fk = d.pk), fact_x_idx (x = 1)
            ->  Seq Scan on dim d
                  Filter: (region = 'eu'::text)

and with ANALYZE, besides §10's counters, `Join Keys Looked Up` (dimension rows with a non-NULL
key) and `Join Keys Without Entry`.

### Declined in v1, and why

- **A non-unique dimension key** (above: a scope and costing decision, not a correctness one).
- **A partitioned fact table.** §16 opens one partition at a time, so the join would have to re-run
  the dimension child per partition or look every key up in every partition's index; both are
  straightforward but double the tested surface. A partitioned, inheritance or subquery DIMENSION
  is declined for want of an index list to prove uniqueness from; `innerrel_is_unique()` would
  prove some of those and is the natural v2.
- **More than two relations**, composite keys, snowflake chains: the dimension would be a join
  itself.
- **Fact columns in the output** (`GROUP BY d.attr, f.x`): per dimension row that is a §10 GROUP BY
  over `f.x`, which composes, but is not in v1.
- **`count(f.col)` of a nullable column, and every non-count aggregate**, as for a single table.
- **Semi and anti joins** (`WHERE f.fk IN (SELECT pk FROM d WHERE ...)`): the IN form is this count
  over the subquery's distinct keys, but it arrives as a SpecialJoinInfo and is left for later.

### Tests

`test/sql/fkjoin.sql`: every query against the same query with the pushdown off, as a multiset both
ways round, on a clean heap and on a dirty one (deletes and updates of fact and of dimension rows
before VACUUM) - GROUP BY one and two dimension columns and an expression, the ungrouped count with
a dimension filter, fact filters (`=`, IN, `IS NULL` on another column, OR), NULL fks, NULL
dimension keys and a NULL group, HAVING, ORDER BY and LIMIT on top, cross-type keys (`int4` fk
against `int8` pk and `int8` against `int4`), text keys, `count(1)` and `count` of either join
column, generic plans with Param fact and dimension filters (a NULL one included), and a correlated
subquery whose dimension filter is an exec Param, so that the node and its child are rescanned per
outer row; the declines - a non-unique dimension key, a key unique only under another collation
than the join's (and accepted once a unique index under the join's own exists), a second join
clause, an outer join, three relations, a fact filter the posting sets cannot answer, a dimension
key pinned to a constant, fact RLS, a fact column in the output, a volatile grouping expression,
another aggregate, `count` of a nullable fact column, a partitioned fact and a partitioned
dimension; the cost model's choice with nothing disabled, including a 50,000-row dimension it must
refuse unless its own quals leave few rows; dimension RLS applied (a policy hiding rows changes the
answer exactly as it does the ordinary plan's) and column privileges on `d.attr` enforced; the
`Join Keys Looked Up` / `Join Keys Without Entry` counters (NULL keys not looked up; keys whose
entries VACUUM deleted); EXPLAIN through `lion_explain_norm()`. No new concurrency argument is
introduced - the fact side is §9 per dimension row and the dimension side is a core scan under the
same snapshot - so no isolation spec is added.

## 28. Range predicates over the sorted directory (v1, implemented)

Formerly §12 item 5. `k < v`, `k <= v`, `k >= v`, `k > v` and `BETWEEN` on a scalar lion column,
answered by the bitmap scan and by the count pushdown. What §21 made possible is the order: within
one key column and one kind the directory is sorted by proc 4 first (`(attno, kind, proc 4, hash,
bytes)`), so the entries a range selects are one contiguous run of VALUE entries, and a range is a
descent to where that run begins and a leaf walk to where it ends.

### Strategies and opclasses

Four new strategy numbers, in btree's order and after the five §17 uses: **6 `<`, 7 `<=`, 8 `>=`,
9 `>`** (`LION_STRAT_LT` .. `LION_STRAT_GT`; `amstrategies` 5 -> 9). They are added in place - 0.1
is unreleased - to every SCALAR class of `pg_lion--0.1.sql` that has proc 4, and to `citext_ops`:

- `integer_ops` and `float_ops` get the four operators for every type pair they already have an
  equality and a cross-type proc 4 for (`<(int4, int8)` beside `btint48cmp`, and so on), so
  `int4col < 5000000000::int8` descends exactly as `int4col = 5::int8` does;
- every other ordered class gets its own type's four: oid, bool, char, name, text (varchar through
  binary coercion, as for `=`), bpchar, bytea, uuid, date, time, timetz, timestamp, timestamptz,
  interval, numeric, macaddr, macaddr8, inet, jsonb, pg_lsn, xid8, tid, enum; and citext. enum_ops
  is declared FOR TYPE anyenum, so a bound of the column's own enum type is the class's own type,
  not a cross-type search (`lion_range_add()`, `lion_match_index()`); the count pushdown of an enum
  EQUALITY still does not make that step and is declined, as it was before this section;
- `xid_ops` and `cid_ops` get none: they have no proc 4, their directory is in hash order and a range
  has no run to walk. `array_ops` and `tsvector_ops` get none: their entries are extracted keys, not
  column values, and `tags < '{a}'` compares whole ARRAYS.

The script rules of 7c6e439 hold: the operators are core's, which an unqualified operator name in an
extension script finds in pg_catalog first; citext's are named `@extschema:citext@.<` and so on,
never through the target schema (`test/sql/hardening.sql` plants a `<` there and shows it is not
the one chosen). Cross-type date/timestamp comparisons (`datecol < now()`) are not added: the lion
date and timestamp classes have no cross-type EQUALITY either, and a family gains a type pair whole
or not at all.

**`lionvalidate()`** accepts strategies 6..9 in a scalar family only for a type pair that ALSO has
proc 4 - the walk needs the comparison, and an operator the index could only answer by testing every
entry is a promise the AM should not make - and never in a multi-key family. Equality stays required
for every type pair the family knows, so a range-only pair is still refused.

**What `k op v` means to the index** is what it means to btree: the rows whose key compares so under
the family's proc 4 for (key type, v's type), under the INDEX's collation. For these families that
comparison IS the operator's (both are core's btree support), which is the promise §21 already asks
of proc 4 about strategy 1; an opclass author whose proc 4 disagrees with its `<` gets wrong answers
from btree as well. Collation is the planner's rule for equality, unchanged: the bitmap scan is only
offered where `IndexCollMatchesExprColl()` holds, and the pushdown makes the same test in
`lion_match_index()`. NULL keys never satisfy a range: the walk visits VALUE entries only, so the
reserved NULL (and EMPTY) entries are never in one.

### The bounded walk (`LionRange`, lion_count.c)

A column's range clauses become one `LionRange`, resolved once per scan (per partition, §16):

- **Each bound's comparison** comes from §21's probe resolution, `lion_probe_init()` for the bound's
  type, so the walk and the lookups cannot disagree about the order: the column's own proc 4 for its
  own type or a binary coercion to it, the family's cross-type proc 4 otherwise. A bound for which
  that resolution finds no comparison - an UNORDERED column, or a family with a cross-type `<` and no
  cross-type proc 4, which the validator refuses but a hand-edited catalogue could hold - is tested
  with the OPERATOR itself instead, entry by entry over the column's whole run: correct and linear,
  and never what a shipped class does - but it is what a user class whose proc 4 has a SQL-standard
  body gets, since a build lays such a column out in hash order (§21). Whether the column is
  ordered is the RECORDED order of §21 ("The order is the index's"), never the catalog's of the
  moment; §21 also says why that record is a best-effort guard against a comparison changed after
  the build, as it is for btree, and not a guarantee. Like every comparison of a
  descent, these calls run with a leaf share-locked; §21 says why a SQL-language operator that
  reads the same index can hang its own backend there, and why that is a superuser's concern.
- **Positioning.** When every bound has a comparison and the column is ordered, the walk descends to
  the first entry of the column that does not sort below the first LOWER bound: a search key of kind
  VALUE with that bound as its key, hash 0 and no stored form, which compares as the smallest member
  of its run (§21), so the descent lands on the first entry whose proc 4 is at or above the bound.
  Without a lower bound it starts where the column starts. Either way the scan's resume position is
  the column's (attno, MINF), so the entries of an earlier column on the landing leaf are passed by
  the ordinary resume comparison and the bound needs no special case there.
- **The test per entry.** A reserved entry is skipped. An entry failing a LOWER bound is skipped - the
  order leads with proc 4, so those are a prefix of what is walked: the landing leaf's entries below
  the bound, and those below any lower bound other than the one descended with. The first entry
  failing an UPPER bound ends the walk. With operator tests nothing ends it early.
- **Several bounds on one column are ANDed into one walk**: `BETWEEN a AND b` is `k >= a AND k <= b`,
  one descent to `a` and a walk that stops past `b`. An empty range (`k > 5 AND k < 5`), an inverted
  one (`BETWEEN 9 AND 3`) and bounds outside the key domain need no special case: the walk starts
  past its end, or runs off the column, or the cross-type comparison says every key is below
  (`btint48cmp` compares an int4 key with an int8 bound exactly, so `int4col < 5000000000` is every
  row and `int4col > 5000000000` none, as with btree). NaN sorts above every float in
  `btfloat8cmp`, as `float8lt` orders it, and -0 ties with 0, whose entry is one (hashfloat8
  agrees). A NULL bound - a Param that came out NULL - selects nothing, every operator involved being
  strict.

Resuming, splits, deletes: the walk IS §21's entry scan (`lion_entry_scan_begin_range()`), which
resumes at "the first key above the last one returned" and so already survives splits and VACUUM's
entry deletion; what differs is only the leaf it reads first, which a descent chose and which may
have split since - its entries then moved right, where the walk goes next, and a directory leaf is
never unlinked (§21). The bitmap walk is `lion_emit_all_keys()`'s copy-a-leaf walk with a start and a
stop. So there is **no new concurrency argument**. An isolation spec is added all the same
(`count_range_vacuum_race.spec`, below), because the per-entry §9 argument of the count is the thing
a reviewer will want to see exercised under a range.

### Bitmap scans (`liongetbitmap()`)

A scan key with strategy 6..9 on a scalar column is a RANGE key. The per-column ranking of §24
becomes: equality 0, list 1, range 2, null tests 3 - and every range key of the chosen column is
consumed by the one walk, so `BETWEEN` needs no recheck. A column with an equality or a list as well
answers that and leaves its range keys to the heap recheck (they can only shrink the result).

The walk emits entry by entry: an INLINE payload from the leaf's private copy, a posting tree through
`lion_emit_chain()` one page at a time, with nothing pinned between entries. A range of any width
therefore holds at most one index pin at a time and draws nothing from §15's pin budget - the bitmap
scan needs no §9 interlock at all, since the executor visits every TID it emits - and its memory is
the TIDBitmap's own `work_mem` budget, which goes lossy rather than growing.

`k < ANY (array)` arrives as an array key with a range strategy (amsearcharray is on for the whole
family); only `ANY` is ever an index qual. The union of `k < e` over the elements is `k < max(e)`, so
it is ONE walk to the widest element - the largest for `<` and `<=`, the smallest for `>=` and `>`,
found with the probe's comparison of the array's type (§21) - and only a column with no such
comparison walks once per element. NULL elements are skipped (a strict comparison with NULL is never
true, and neither is `ANY` of NULLs and falses); an empty or all-NULL array selects nothing.
*(Deviation from the first version, which walked every element into the same bitmap: 200
near-equal bounds over a million-row unique column emitted ten million TIDs, 1.4 s against 20 ms
(2026-09-24 review). One walk now emits the 432,200 matching rows once, at the cost of the single
widest bound, 108 ms on the assert build.)* `lioncostestimate()` charges such a column the entries
of that one walk, as it does a plain range.

On a MULTICOLUMN index (§24) a range column cannot be a node of the set tree - its answer is a union
of an unbounded number of entries - so it is answered into a TIDBitmap of its own and INTERSECTED
(`tbm_intersect()`) with the bitmap of the other columns' tree, and the result is OR-ed into the
caller's bitmap, which a BitmapOr above may share with its other arms. Two range columns are two such
bitmaps. It is what core's BitmapAnd does, inside one index scan.

### amcostestimate

`genericcostestimate()` already charges what the walk reads in proportion to the selectivity - that
share of the index's pages at random_page_cost and of its tuples at cpu_index_tuple_cost - which is
about the leaves and the posting pages in the range. On top of that the walk is charged per ENTRY:
`n_distinct(k) x selectivity` of them, each a decode and one comparison per bound. On a low- or
mid-cardinality column that term is small and the index is a fraction of a btree's, so the lion
bitmap scan wins; on a near-unique column every row is an entry, the index is larger than the btree
(an entry header per row) and the entry term adds to the per-row charge, so btree - which also has a
correlation to exploit, and a lion scan never has one - wins. `test/sql/range.sql` pins both choices.

### Count pushdown: a range BOUNDS the driver, it is never a source

The design question was whether a range should be a count SOURCE - the union of its entries, merged
k-way like an IN list (§15) - or a restriction on the entry walk that DRIVES the count. It is the
second, and the reason is §15's own argument: the entries of one scalar column are disjoint, so the
rows whose `k` is in a range are the disjoint union of the range's entries, and

    count(*) WHERE k IN range AND F  =  sum over the entries e in the range of |e ∩ F|

which is §14's sum-over-all with the walk bounded, while `GROUP BY k WHERE k IN range AND F` is
§10's group walk bounded the same way. The union has no plan-time size (an IN list's is capped at
1000 at plan time; a range has whatever entries the data holds), needs every entry located - a pin
each under §15's budget, NOPIN re-locates and all - and pays a k-way heap per count. The driver form
holds ONE entry's pin at a time plus the WHERE sources', which is §10's GROUP BY and needs no new §9
argument: each entry is counted as a single-key count is, intersected with the WHERE sources, and the
pin on the page its containers came from is kept until they have been through the visibility map.
The WHERE sets are materialized on their second use as for any GROUP BY (§9), so a selective filter
is probed in memory, entry after entry.

Accepted (`lion_try_count_path()`), every other rule of §10, §14 and §16 unchanged:

1. **`count(*)`, `count(k)` or `count(col)` under §14's rules, `WHERE <range on k> [AND ...]`**
   without a GROUP BY: the sum-over-all driver (`LION_FLAG_SUMALL`) with `k` as the driving column
   and the walk bounded (`LION_FLAG_RANGE`). `count(k)` is answerable because a range proves `k`
   non-NULL (the operators are strict). Clauses on other columns are the ordinary sources, an
   `IS NOT NULL` on another column included, and a `k IS NOT NULL` beside the range is dropped as
   §14 drops the driver's own.
2. **`GROUP BY k WHERE <range on k> [AND ...]`**: the group walk bounded. The value-representation
   contract of §10 applies to emitting `k` as it does without a range, and so do the pathkeys of §21
   - the walk is still in key order.
3. **`count(DISTINCT k) WHERE <range on k> [AND ...]`** (§26 shape 1, bounded), and
   **`g, count(DISTINCT k) ... WHERE <range on g> GROUP BY g`** (§26 shape 2 with its OUTER walk
   bounded). A folded single group does not ask k's NULL entry whether the group exists when the
   range is on k, because the range excludes exactly those rows.
4. **Partitioned tables** (§16): shapes 1 and 2, with the range resolved against each partition's own
   index, key column and heap numbering.
5. **Params as bounds** (generic plans, nested-loop exec Params): evaluated at scan start with the
   other clause values (§10); a NULL one makes the count 0, and a GROUP BY empty.
6. Any number of range clauses on the SAME column, combined into one walk. They are exempt from the
   one-positive-clause-per-column rule among themselves, and from nothing else.

Declined, and why:

- **A range on a column that does not drive**: `g, count(*) ... WHERE <range on k> GROUP BY g`,
  `count(DISTINCT g) WHERE <range on k>`, a range beside a two-column GROUP BY, two range columns.
  Each needs the range as an intersected SOURCE - the union above, or a §20-style (g, k) pair loop
  summed per group - which are both real designs with their own costs (pairs = |G| x entries in
  range) and are §23 follow-ups rather than something to ship half-priced. The ordinary plan answers
  them, now with a lion bitmap scan for the range.
- **A range under an OR** (§19): the same union, inside another union.
- **A range beside `=`, `IN` or `IS NULL` on the same column**: two positive clauses on one column,
  which §10 already leaves to the ordinary plan.
- **A range as an FK-join fact filter** (§27): the fact filters are sources ANDed with every fk set,
  so this is the union again.
- **min(k)/max(k)** (§23). Core's own min/max optimisation (planagg.c) needs an ORDERED index scan -
  `amcanorder` and `amgettuple` - which lion does not have. Doing it here means a new node shape that
  PRINTS a value (the value-representation contract), walks from either end to the first entry with a
  visible row (an existence test per entry, §26), and for max walks backwards, which §21's leftlink
  allows on paper and nothing reads yet. It fits, but not cleanly enough to ride along with this.

**Cost** (`lion_cost_count_rel()`). The range clauses are not sources: no lookup, no chain and no
container term of their own. They scale the DRIVER: the entry scan's pages by the fraction of the
column's entries in range - the combined selectivity of the range clauses alone
(`clauselist_selectivity()`), which is the right fraction for a column whose rows are spread evenly
over its values and an overestimate of the walk for a skewed one - and the entries walked
(`numgroups` of the sum-over-all and distinct walks) by the same fraction of `n_distinct(k)`.
Everything per entry is then priced as the unbounded walk prices it, plus a fixed cost per entry
walked, `LION_RANGE_ENTRY_COST` (40 `cpu_tuple_cost`): the walk resumes at a key (a leaf read and a
binary search), copies the entry out and sets up and tears down a merge for it. Measured on the
assert build at 100k rows over a unique timestamp, 3601 entries took 6.7 ms summed and 5.2 ms
grouped - 1.4 to 1.9 us each - against the btree index-only scan's 0.64 ms for the same rows at 171
cost units; without the term both were chosen at about a tenth of that estimate, with it the
near-unique column goes to btree and a range over a 200-value column (21 entries, 0.18 ms against
btree's 0.9) stays with the node. A count(DISTINCT) walk already pays §26's per-test cost for the
same work and does not pay this one too. *(Deviation from this section's first draft, which
expected the unbounded walk's per-entry terms to be enough; the unbounded group walk of §10 keeps its
own calibration, which its own tests pin.)*

**The dirty heap is priced per entry too.** §10 charges each dirty page ONCE per query, because the
visibility cache answers every later visit - but the cache resolves a page on its SECOND visit, and a
range walk counts entry by entry, each count flushing its own recheck batch, so a page holding rows
of several entries is fetched for each of them until the cache has it. At five million rows with
every heap page dirty, a 30-day range over randomly placed days rechecked 68,384 block visits for
37,133 pages (166 ms, against the bitmap heap scan's 46 ms) and was chosen; the same range over days
stored in order rechecked 581 (3.8 ms, against the btree's 11.5) and was refused. So for a ranged
walk the recheck pages are `entries x the pages one entry's candidates lie on x dirtyfrac`, the
per-entry pages interpolated between one per row (values scattered) and the rows' share of the heap
(values in heap order) by the squared correlation, as `cost_index()` does, and never more than twice
§10's figure, since the cache stops a page being fetched a third time while it has room. Both
choices above are now the faster plan. The correlation's square is harsh on NEARLY ordered data: a
100k-row column whose correlation updates had brought down to 0.96 was priced at 960 page visits
against the 154 measured, and btree's 1.7 ms was chosen over the node's 0.55. That is the safe side
of the error, and the §23 fix below - carrying the recheck batch across a sum's entries, as §15's
disjoint sum does across a list - would make the question moot.

**EXPLAIN** prints the driver with its range in place of `(k)` / `(all keys)`:
`Lion Indexes: ix_d (d >= '2024-01-01'::date AND d < '2024-02-01'::date), ix_s (s = 3)` - a Param
as `$1` - and `Group Key` and `Distinct Key` as before. `Directory Pages Read` (§21) shows the walk
is bounded.

**Privileges** (§9). The range operators' functions (`int4lt`, `date_ge`, ...) are WHERE-clause
functions, so `lion_replaced_functions()` already lists them in `LION_PRIV_EXECUTE`, and the node
checks EXECUTE on them at startup as the ordinary plan's scan does. `test/sql/range.sql` revokes
`int4lt` from PUBLIC and gets core's error from both plans.

### Measured (2026-09-24, the distinct slot's PostgreSQL 20devel, assert-enabled: ratios, not absolute numbers)

Five million rows over 37,133 heap pages, all-visible after VACUUM: `d` - about 2000 days stored in
order (2500 rows a day), `dr` - the same days in random order, `status` - 20 values, `ts` - a
timestamp unique per row and in heap order, each with a lion and a btree index (lion `d` 320 kB
against btree's 33 MB, `dr` 47 MB against 33, `status` 10 MB against 33, `ts` 325 MB against 107).
Best of five `EXPLAIN ANALYZE` runs, milliseconds; "chosen" is the plan with nothing disabled, the
other columns force one path each (the btree column is its best plan, an index-only scan for a
count):

| query | chosen | LionCount | lion bitmap | btree | seq |
|---|---|---|---|---|---|
| `count(*) WHERE d` 1 day | node 0.19 | 0.19 | 0.47 | 0.26 | 237 |
| `count(*) WHERE d` 30 days | node 0.24 | 0.23 | 7.14 | 5.78 | 236 |
| `count(*) WHERE d` 365 days | node 0.93 | 0.90 | 85.6 | 69.9 | 263 |
| `count(*) WHERE d` 2000 days | node 3.08 | 3.00 | 351 | 288 | 354 |
| `count(*) WHERE dr` 1 day | node 0.25 | 0.24 | 8.48 | 0.26 | 252 |
| `count(*) WHERE dr` 30 days | node 2.31 | 2.43 | 57.3 | 5.84 | 253 |
| `count(*) WHERE dr` 365 days | node 25.9 | 26.0 | 187 | 70.7 | 286 |
| `count(*) WHERE dr` 2000 days | node 106 | 105 | 480 | 285 | 372 |
| `... dr` 30 days `AND status = 3` | node 4.42 | 4.41 | 30.4 | 32.5 | 261 |
| `dr, count(*) ... GROUP BY dr`, 30 days | node 2.21 | 2.14 | 68.6 | 7.55 | 266 |
| `count(DISTINCT dr)`, 365 days | node 1.33 | 1.29 | 267 | 73.3 | 357 |
| `sum(x) WHERE d` 7 days | btree index scan 2.66 | - | 2.07 | 2.59 | 237 |
| `sum(x) WHERE dr` 7 days | btree bitmap 27.1 | - | 27.2 | 26.8 | 250 |
| `count(*) WHERE ts` 1 hour | btree IOS 0.10 | 0.41 | 0.32 | 0.09 | 258 |
| `count(*) WHERE ts` 1 day | btree IOS 0.48 | 3.88 | 0.98 | 0.47 | 256 |
| `count(*) WHERE ts` 30 days | btree IOS 11.2 | 109 | 20.4 | 11.1 | 261 |

The count pushdown is chosen wherever it wins - by one to three orders of magnitude over the btree
for a clustered column and three to thirty times for a scattered one - and a near-unique column goes
to the btree, where the node would be four to ten times slower. The lion BITMAP scan is at parity
with the btree's where the heap dominates (a `sum(x)`), and the planner picks between them on cost.
With 1% of the rows updated so that every heap page is dirty: `d` 30 days, the node 3.5 ms (chosen)
against the btree's 11.5; `dr` 30 days, the node 157 ms against the bitmap heap scan's 58 (chosen);
`dr` 30 days `AND status = 3`, the node 18 ms (chosen) against 30; `count(DISTINCT dr)` over 365
days, the node 22 ms (chosen) against 263.

### Tests

`test/sql/range.sql`, written before the code and shown failing first: every strategy and
combination (`<`, `<=`, `>`, `>=`, `BETWEEN`, open-ended, empty, inverted, equal bounds, bounds
outside the key domain), commuted forms (`5 < k`), cross-type int2/int4/int8 and float4/float8 with
-0, NaN and Infinity, text under the default collation and under `"C"` with a mismatched collation
declined, citext, numeric, date/timestamp/timestamptz, uuid, bool, enum; NULL keys excluded; a
multicolumn index (a range on one column and an equality on another, and two ranges); a partial
index; after deletes and updates (a dirty heap) and after VACUUM (a clean one); every answer equal to
a sequential scan's. The count pushdown for each accepted shape with its plan and exact answers, each
decline, Params under `force_generic_plan` (a NULL one included), a partitioned table, the index pins
of a GROUP BY parked mid-walk (pg_buffercache: one entry's, however wide the range), EXECUTE revoked
on a range operator, and plan choice with nothing disabled: a lion bitmap scan over btree for a range
on a low-cardinality column, and btree or a sequential scan over lion on a near-unique one. EXPLAIN
goes through `lion_explain_norm()`. `test/isolation/count_range_vacuum_race.spec` parks a range count
with an entry's container pinned while the VACUUM of that entry's dead rows waits, as
count_vacuum_race.spec does for an equality.

## 29. Index scans: amgettuple, ordered scans, index-only scans

Until this section lion answered only bitmap scans. Measured on release PG18.6 at 98ecedd
(bench/results/2026-09-24-98ecedd-*), row fetches that visit the heap anyway were at parity with
btree except in two shapes that only a plain Index Scan serves: a very selective lookup
(`WHERE c1m = ...`, one row: btree 0.023 ms against lion's bitmap 0.027 ms at 1M rows, the
difference being the bitmap's fixed setup) and a low `work_mem` (64 kB: `WHERE c200 = 17`, 0.5% of
5M rows, btree Index Scan 13 ms against a LOSSY lion bitmap 92 ms, which rechecks every row of
every lossy page). A lion index could not do better because it had no `amgettuple`. This section
adds it (step 1, implemented), and records what ordered scans (step 2, deferred to the backlog) and
index-only scans (step 3) would need from it, so that neither has to redesign the scan.

### 29.1 Handler settings (step 1)

    amgettuple = liongettuple (every supported major, 16 .. 20)
    amcanbackward = false       ammarkpos/amrestrpos = NULL     amcanparallel = false
    amcanorder = false          amcanreturn = NULL              amsearcharray = true (unchanged)
    amsearchnulls = true (unchanged)                            amoptionalkey = true (unchanged)

- **No backward scans.** `amcanbackward = false` is what `ExecSupportsBackwardScan()` reads, so a
  SCROLL cursor over a lion Index Scan gets a Material node from the planner, and a NO SCROLL one
  refuses `FETCH BACKWARD` in the executor before the AM is asked. The only other source of a
  backward index scan is an ORDERED index (indxpath.c builds a backward path for every index with
  a sort order, whatever `amcanbackward` says), which lion is not; `liongettuple()` therefore
  raises an internal error for any direction but forward, and nothing can reach it.
- **No mark/restore.** A merge join over a lion Index Scan would need one only if lion claimed an
  order, and it claims none; with `ammarkpos` NULL the planner puts a Material node under a merge
  join that needs to restore anyway.
- **`kill_prior_tuple` is ignored** in this version. btree uses it to set LP_DEAD on index tuples
  whose heap tuples are dead to everyone; lion has no per-TID flag to set (a TID is a bit in a
  container), and removing the bit would be a write under a share lock on a page other scans are
  reading. VACUUM removes them. Known cost: a plain scan over a hot key with many dead versions
  keeps visiting their heap slots until the next VACUUM, as a bitmap scan does today.
- **No parallel scan.** `amcanparallel` stays false; the parallel callbacks stay NULL.
- **No returnable column** (`amcanreturn` NULL, step 3) - but an index-only scan of a query that
  needs no column at all is possible the moment `amgettuple` exists, and it is served (§29.9).

### 29.2 The scan plan, shared with the bitmap path

Both entry points begin with the same per-column choice `liongetbitmap()` always made (§5 SCAN
step 5, §24, §28), factored out as `lion_scan_choose()`: per key column the most selective-looking
qual - equality (rank 0), list (1), range (2), null test (3) - is answered, every other qual of
that column is left to the heap recheck, and all range keys of a column are ONE walk when the
range is the chosen qual. The per-column set trees (`lion_scan_col_tree()`: an equality is a leaf,
an IN list an OR over its located sets, `IS NULL` the reserved NULL entry, a multi-key query its
extracted tree, §17) are built by the same function for both paths. What differs is only how the
answer is delivered: into a TIDBitmap, or as a stream the executor pulls one TID at a time.

### 29.3 The TID source (`LionSource`, lion_scan.c)

A plain scan is a *source* of TIDs, opened on the scan keys at the first `amgettuple` call after
`amrescan`. It has one of five shapes:

- **NONE**: a chosen qual selects nothing (`col = NULL`, an empty or all-NULL list, a key absent
  from the index, a multi-key query in mode NONE). `amgettuple` returns false at once.
- **SETS**: every chosen qual is a set tree. The columns' trees are ANDed (§24) and the whole
  expression is ONE stream of containers in ascending container key, produced by the very
  evaluator the count and the bitmap path already share (§9 cursors, §15 k-way OR, §22 leapfrog
  AND: `LionExprCursor`). An IN list is the OR of its sets - a union by container key - so the TIDs
  come out in HEAP order, each exactly once, and the heap is visited in physical order, one pass.
  Equality, IN, `IS NULL`, multicolumn ANDs and multi-key queries (arrays, tsvector) in mode KEYS
  all take this shape.
- **WALK**: one scalar column's chosen qual is a range (§28, including `op ANY (array)`), an
  `IS NOT NULL`, or the scan has no key at all (a partial index whose predicate the query implies,
  §24, which walks column 1 with its NULL entry). The column's entries are walked in directory
  order exactly as the bitmap walk does (`lion_emit_all_keys_ext()`: copy a leaf, release it,
  test each entry with `lion_range_test()`, stop at the first entry past an upper bound or of the
  next column), and each selected entry is streamed as `AND(entry, rest)`, where `rest` is the set
  tree of the other columns' chosen quals (empty for a one-column scan). Within one entry the TIDs
  come out in heap order; across entries in key order. A second walk column is dropped and
  rechecked: a range column cannot be a set-tree node (its answer is a union of an unbounded
  number of entries), and the bitmap path's answer for two range columns - one TIDBitmap each,
  intersected - is not a stream. An `op ANY (array)` range walks to the widest element (§28) when
  the column orders the elements; an UNORDERED column walks ONCE, testing each entry against every
  element's range (the bitmap path walks once per element and lets the bitmap absorb the overlap,
  which a stream cannot).
- **LIST**: an IN list on a scalar column longer than a batch (§29.4) is located and streamed a
  batch at a time, each batch the SETS shape with the other columns' trees ANDed in. It outranks a
  walk, which is then left to the recheck, and a second long list is left to the recheck too.
- **UNION**: a MULTI-KEY column would have to be read whole - `IS NOT NULL`, a query in mode ALL
  (§17) with nothing else expressible, or a no-key scan whose first column is multi-key. A row is
  under several entries of such a column, so walking them would return it several times. The
  scan streams the exact UNION of the column's entries instead, a WINDOW of container keys at a
  time: every entry is sought to the window's first key and read to its end, and ORed into one
  bitset image per container key of the window; the next window starts at the smallest key any
  entry had past this one. Each window walks the column's entries once, so the work is windows x
  entries, and the window is therefore WIDE whatever `work_mem` says: `lion_union_window()` =
  `max(1024, work_mem / 4 kB)` container keys (1024 are 65536 heap blocks, 512 MB of heap), up to
  65536, and a key's 4 kB image is made only when an entry has a container there, so a window
  costs what it holds - at most 4 MB at the floor, which is the one place this scan may exceed a
  tiny `work_mem`. Always rechecked (the quals of mode ALL need it). Correct and expensive, which
  is what the cost model already says about these quals; `lioncostestimate()` also charges each
  window past the first another read of the index (the IndexPath is shared with the bitmap scan,
  which is overcharged by that, only on a heap past 512 MB).
  *(Deviation from the version before it, whose window was `max(16, work_mem / 4 kB)` keys: at
  64 kB that is 1024 heap blocks, and a 2M-row table with 200k distinct keys took 17 windows and
  1.40 s for an index-only count(*) against 0.48 s at 64 MB - and the planner picked that scan at
  64 kB; 100M rows would have meant some 1600 walks of every entry (re-review, 2026-09-24).
  Keeping every entry's cursor across windows instead would have made the memory grow with the
  number of distinct keys, and identifying an entry across windows, while inserts and VACUUM change
  the directory between two calls, needs its key - both worse than a wider window.
  `test/sql/indexscan.sql` §10 counts the index blocks one count reads at 64 kB and at 64 MB: the
  same, where they were five times as many at 64 kB.)*
  *(Deviation from this section's first version, which ran `liongetbitmap()` into a private
  TIDBitmap and, on a LOSSY page, returned every offset up to `MaxHeapTuplesPerPage` "with recheck
  set". That is right for a bitmap heap scan, which re-applies the index PREDICATE to a lossy
  page (bitmapqualorig), and wrong for a plain Index Scan, whose recheck quals are the index quals
  minus what the predicate implies, and for an index-only scan, which rechecks nothing: on a
  partial `(tags) WHERE flag` index at 64 kB of work_mem, `count(*) WHERE flag` answered 175,076
  for 100,000 and a plain scan returned 140,216 rows with `flag` false - the 2026-09-24 review.
  The rule that fixes it is §29.6's first one: no shape returns a TID the index does not hold.)*

Scalar entries of one column are disjoint (one entry per equality class, §21), so a WALK never
returns a TID twice either, and neither does SETS, whose OR node merges by container key.

**Why an IN list is a union and not a walk of its entries.** With no order to keep (§29.8), the
union is strictly better for a plain scan: it visits each heap page once, in physical order, while
an entry-by-entry list restarts at the beginning of the heap for every value; and it is already the
shape the bitmap path evaluates. An ordered scan would need the other one (§29.8).

**The stream** (`LionSetStream`, lion_count.c: `lion_stream_begin()`, `lion_stream_next()`,
`lion_stream_end()`) is the `LionExprCursor` behind a pull interface - `lion_sets_iterate()`, the
push form the bitmap path uses, is now a loop over it - with one new switch, `keeppins`, described
in §29.5. A walked entry becomes a `LionPostingSet` without a lookup: an INLINE entry's payload
points into the walk's private copy of the leaf, a CHAIN entry's `head` is its posting tree's root.

### 29.4 Batches and memory

**Every located set costs its cursor, and an IN list is located whole** - which the first version
of this section missed: a located set with the cursor that reads it takes ~20 kB (the cursor's
4 kB staging container, its share of the OR node, the set), so a 20,000-value list held 220 MB and
`c1m = any(array(select ... 300000 ...)) LIMIT 10`, planned as a plain Index Scan, peaked at
1.37 GB to return ten rows (2026-09-24 review). The bitmap path never had it: `lion_emit_array()`
looks its values up one at a time. So a list longer than a BATCH - `max(32, work_mem / 32 kB)`
values, which keeps a batch's cursors inside `work_mem` - is the LIST shape: its values are sorted
once into lookup order (`lion_probe_sort()`: the probe's comparison, then the hash), and located
and streamed a batch at a time, the previous batch's sets and pins gone before the next is located
(the §11 order). A batch is cut only where the HASH changes, and every value of one equality class
compares equal and hashes alike, so the class's values sit next to each other and never straddle
two batches: no entry is located twice, and since distinct entries of a scalar column are
disjoint, no TID is returned twice - citext's `'Alice'` and `'alice'` included. What stays is the
list itself, sorted: 12 bytes per value next to the executor's own array. The heap is visited in
physical order within a batch and restarts per batch, which is what a btree scan of the same list
does per value. No new cost term: with batches the plain scan's work is linear in the list, like
the bitmap's, and a list that is a Param - the reviewed case - has no length the planner could
price anyway. Not covered: a multi-key column's `op ANY (array of queries)` still extracts every
query up front (per query, not per row; arrays of queries are rare), and the BITMAP path's
multicolumn intersection (`lion_emit_columns()`) still locates a list whole, as it always has.

`amgettuple` returns TIDs out of a BATCH: the members of the container the stream is standing on,
expanded into an array of at most `LION_CONTAINER_RANGE` lo values (`lion_container_to_array()`,
turned into a TID one at a time, in the per-heap-block order `lion_container_to_tbm()` emits). The
next container is pulled only when the batch is used up. What the scan holds between two calls is
therefore bounded by `work_mem` (save a UNION window's floor) and the query, never by the data: one container's members; one
8 KB image per posting-set cursor of the tree (the page its current container came from), for at
most a batch of an IN list's sets (above); the INLINE payload copies of those sets (at most an
entry each, `LION_MAX_ENTRY_SIZE`); for a WALK one directory leaf image; for a UNION the bitset images
of its window (made on first use; up to 4 MB at the floor, §29.3); and a long list's values, sorted. A posting set of any size is
STREAMED container by container - a 5M-row entry is never materialized - and a walk of any width
holds one entry's stream at a time. Memory lives in a per-scan context reset by `amrescan`, and a
walk's per-entry cursors in a per-entry context reset before the next entry, so a nested-loop inner
scan rescanned a million times and a range over a million entries both run in constant memory.

### 29.5 Pins, and what a scan paused between calls may hold

The executor returns a row to its caller between two `amgettuple` calls, so a scan can be paused for
any length of time - a cursor held open by a client, a nested loop's outer side, a LIMIT that never
comes back. The rules: no LWLock is held across calls, ever; a PIN may be, and whether it is
depends on the snapshot, as in btree.

**MVCC snapshots drop every pin** (`dropPin`, mirroring nbtree's `so->dropPin`, set in btrescan,
and `_bt_drop_lock_and_maybe_pin()`; nbtree/README "Making concurrent TID recycling safe"): the
scan copies what it needs and holds no pin at all while the executor has the row. With `keeppins`
off every cursor releases a posting leaf the moment it has copied it, and a located or walked
INLINE set is unpinned as soon as its payload is copied (`lion_posting_set_unpin()`, as the bitmap
path already does). The condition is btree's: `IsMVCCLikeSnapshot()` (`IsMVCCSnapshot()` before
19, which already included historic snapshots), `!xs_want_itup` (always true in step 1: there is
no `amcanreturn`) and a heap relation. This matters beyond speed: a cursor that pinned a posting
leaf would make every VACUUM of the table wait for it (§11 takes a cleanup lock on every page), for
as long as the client leaves the cursor open.

Why an MVCC scan needs no pin, case by case. What it holds across a pause is private memory only:
copies of posting leaves (each with the right link it had when copied), INLINE payload copies, the
posting-tree root of each CHAIN set, a directory leaf image with its right link, and a batch.

1. *TIDs VACUUM removes after the scan copied them* - the heap fetch applies the snapshot: the
   tuple is dead to it, or its slot is LP_UNUSED, or it was reused by a tuple inserted after the
   old one became dead to EVERY snapshot, ours included, which our snapshot cannot see either.
   That is nbtree's argument verbatim (README, "Making concurrent TID recycling safe": "An MVCC
   snapshot is only sufficient to avoid problems during plain index scans because they must access
   granular visibility information from the heap proper"); a plain lion scan fetches every TID it
   returns, and `xs_recheck` is irrelevant to it.
2. *TIDs VACUUM removes before the scan copies them* were dead to every snapshot, so missing them
   is right.
3. *TIDs inserted after the scan copied a page* belong to transactions our snapshot cannot see
   (an index entry is written before its inserting transaction commits, so every row visible to the
   snapshot was in the index before the scan read it). Missing them is right, and so is seeing them.
4. *A posting leaf splits under a paused cursor* (it holds an image of P with right link R): the
   split moves P's upper items to a new page N between P and R. The cursor already has them in its
   image of P, follows R, and never visits N - nothing missed, nothing twice; what N gains later is
   case 3. R itself splitting is found through R's own right link when the cursor reads R. This is
   §4/§22's rule that items only ever move right, onto a page linked immediately right of their
   source, and it holds whether the new page was extended or recycled (§18).
5. *A root push-down* (§22) moves a one-page set's items to a brand new child: a cursor that copied
   the old root as a leaf has its items; a cursor that has not started (or has a seek pending)
   descends from the root, which `lion_posting_search()` handles at any level.
6. *An INLINE entry spills to a CHAIN* (§4): the scan's payload copy is complete as of the copy;
   case 3 covers the rest.
7. *VACUUM deletes an entry and frees its posting tree* (§18), and the pages are reused by another
   key's set: a cursor that follows its stale right link or descends from its stale root lands on
   a page that is DELETED or claims another owner, and ends the set there - the owner check every
   cursor already makes (`lion_page_owns()`, §18). Ending there is right because VACUUM deletes an
   entry only once its set holds no TID at all. Pages of a LIVE set are never freed (§18: only a
   whole set is), so a set that still has a visible row never loses a page under the scan. (While
   an MVCC snapshot is registered, the freed pages cannot even be handed out again:
   `lion_alloc_page()` takes a DELETED page only once its `safexid` is older than every snapshot,
   §18. The owner check is what the argument rests on; the horizon is a second fence.)
8. *The directory leaf under a paused WALK splits, or loses entries to VACUUM*: the walk holds an
   image of the leaf and its right link, exactly as the bitmap walk does between leaves (§21
   "Ordered iteration", §28): entries moved right are in the image and are not visited again on
   the new page; a deleted entry had an empty set; an entry inserted after the image was taken is
   case 3. Directory leaves are never unlinked or freed (§21), so the right link always names a
   leaf.
9. *The scan's own INLINE copies taken at locate time* (SETS: every set is located at the first
   call) are cases 1 to 3 again.

**Non-MVCC snapshots keep the pin on the page the current batch came from**, as btree's
`!so->dropPin` scans do. `keeppins` on: every cursor keeps the pin on the posting leaf its current
container came from until the stream moves past that container (§9's cursor rule, unchanged), a
located INLINE set keeps its leaf pin for the life of the scan, and a WALK keeps its current
directory leaf pinned while that leaf's entries are streamed. The batch is the current container,
and the stream moves past it only inside the NEXT `amgettuple` call, so every TID is returned and
fetched from the heap while the page it was read from is pinned. VACUUM cannot remove a TID from a
page without a cleanup lock on it, nor from any page after it in chain order before it has had
that page's cleanup lock (§11's guarantee, which is what makes a split or a spill that moves the
TID harmless), and the heap's second pass - the only thing that makes a line pointer reusable -
runs only after `ambulkdelete` has finished on every index. So no TID the scan holds can be
recycled under it: the same property btree gets from its leaf pin, obtained from the protocol §9
already proves for the count. Why it matters: a non-MVCC snapshot (`SnapshotDirty`, `SnapshotSelf`,
`SnapshotAny`) can see the tuple a recycled slot now holds, so without the pin a scan could return
a row under a key it does not have, or return one row twice (once under the old TID's entry, once
under its own). For an AND of several sets one pin suffices (the TID is in all of them, and
`ambulkdelete` has to pass every page of the index), for an OR every child standing at the current
key keeps its own - the same rules §9 states for the count (`lion_source_pinned()`).

- An IN-list set located past the §15 pin budget is NOPIN and carries no interlock. A non-MVCC scan
  that has one sets `xs_recheck`, which filters a recycled slot whose new tuple does not match;
  the one residual effect - the same matching row twice through two stale entries - needs a
  thousand-leaf IN list under a dirty snapshot, which no caller in core builds (below).
- The UNION shape (§29.3) holds no pins in either mode (its windows copy containers out of many
  pages) and is always rechecked; a union cannot return a TID twice. A multi-key query's sets are located unpinned in either
  mode too (the tree builder the bitmap path shares drops their pins), and a multi-key scan is
  always rechecked (§29.6); `EXCLUDE USING lion (tags WITH &&)` is such a scan, and a conflict it
  reports through a recycled slot is a row that really overlaps.
- The §11 deadlock rule for readers holds in both modes: every set is located - every directory
  lock taken - before the first container page is pinned, and a WALK takes the next directory leaf
  only after the previous entry's stream has been closed and its container pins dropped. Holding
  directory-leaf PINS (INLINE sets, the walk's own leaf) while locking directory pages is what the
  count's GROUP BY walk has always done; pins do not block share locks, and VACUUM never waits for
  a cleanup lock while holding any lock (§11).

Who scans a lion index with a non-MVCC snapshot, and the decision for each:

- **Exclusion constraints** (`check_exclusion_or_unique_constraint()`, SnapshotDirty). DefineIndex
  refuses `EXCLUDE USING <am>` for an AM without `amgettuple`, so `EXCLUDE USING lion (k WITH =)`
  becomes possible with this section - an equality-only exclusion constraint, i.e. a unique
  constraint served by lion's single-key lookups. It is allowed: its scan is exactly the pinned
  mode above (one equality key per column, no arrays), the constraint code rescans after waiting
  for an in-progress conflict, and it re-applies the operator whenever `xs_recheck` is set.
  `test/sql/indexscan.sql` tests it, and `test/isolation/gettuple_dirty_pin.spec` shows that the
  pin is held (VACUUM waits for it) where an MVCC scan at the same point holds none.
- **Logical replication, REPLICA IDENTITY FULL** (`RelationFindReplTupleByIndex()`, SnapshotDirty).
  On 16 and 17 `IsIndexUsableForReplicaIdentityFull()` accepts only btree and hash, by AM Oid.
  From 18 it asks `IndexAmTranslateCompareType(COMPARE_EQ, ...)` for every key column and requires
  `amgettuple`; lion has no `amtranslatecmptype`, so today it is never chosen on any version. If
  §29.8 ever adds one, lion becomes eligible, and that is fine: the scan keys are one equality (or
  `IS NULL`, `SK_SEARCHNULL`) per column, which is the pinned SETS shape, and the apply worker
  compares the whole tuple (`tuples_equal()`) for any index that is not the replica identity index.
  Nothing to refuse; a note for whoever adds the translation.
- **SnapshotAny**: CLUSTER needs `amclusterable` (false); nothing else in core scans a user index
  with it. **`get_actual_variable_range()`** (SnapshotNonVacuumable) needs an ordered index with
  `amcanreturn`; lion has neither.

### 29.6 `xs_recheck`

**First, the rule every shape keeps: no TID the index does not hold is ever returned** - only TIDs
that some entry's posting set held when the scan read it, each at most once. `xs_recheck` is a
statement about the QUALS the index was asked to answer, and it is not a license to return a
superset of the index: a plain Index Scan of a partial index has the quals its predicate implies
removed from its recheck, and an index-only scan evaluates no recheck qual at all (the planner only
builds one for a query that references no column, so there is none to evaluate). A row that the
index does not hold therefore comes back as a row. A TIDBitmap's lossy page is the one superset
source lion ever had, and the UNION shape (§29.3) replaced it; every other source is a stream of
posting-set containers.

Then `xs_recheck` is set, for every TID of the scan, when any of these holds; otherwise the TIDs
are exact:

- a qual was not answered: a second qual on a column (§29.2's ranking), a second WALK column, a
  multi-key column in mode ALL next to another column that does answer (dropped, as in the bitmap
  path);
- a MULTI-KEY column's query was answered at all (strategies 2 .. 5, §17). The bitmap path passes
  mode KEYS through unrechecked because `lion_extract_query()` classifies it as exact; the plain
  path is chosen for selective lookups, where one operator call per fetched row costs nothing next
  to the fetch, and it does not have to rest on that classification;
- the UNION shape (§29.3), always;
- a non-MVCC scan with a NOPIN set (§29.5).

A range is exact (the walk applies every bound of the column), an IN list is exact (a union of
whole entries), `IS NULL` and `IS NOT NULL` are exact, and so is a cross-type or binary-coerced
equality (§21's probe). A collation the index cannot answer never reaches the AM (the planner
matches `IndexCollMatchesExprColl()` first).

### 29.7 Rescans, keys that change, and cleanup

- `amrescan` copies the new keys, closes the stream, releases every located set (their pins and
  their pin-budget share, §15), drops the walk's leaf pin, and resets the per-scan context; the
  source is opened again, from the new keys, by the next `amgettuple`. A nested loop's inner index
  scan with a Param (`t.k = outer.x`) is exactly that: one rescan per outer row, one descent per
  rescan. The per-column probe cache (`LionScanCol`, keyed by the key's subtype) survives
  rescans, as it did for the bitmap path.
- `ScalarArrayOpExpr` arrives as ONE key with `SK_SEARCHARRAY`, as for the bitmap path:
  `amsearcharray` stays true because `amgettuple` answers every array key the bitmap path answers
  (equality lists as a union, range arrays as one walk, multi-key query arrays as an OR of their
  trees) - the executor never has to iterate the elements with rescans.
- `pgstat_count_index_scan()` and, on 18+, `instrument->nsearches` are counted once per source
  opened, i.e. once per rescan, as btree counts one per primitive scan.
- `amendscan` does everything `amrescan` does and frees the contexts. An ERROR in the middle of a
  scan leaves pins to the resource owner, which releases them, and the list pin budget is zeroed
  at the end of the transaction (§15).

### 29.8 Ordered scans (step 2): deferred

*Deferred to the backlog (§23) on 2026-09-24, in favour of an ORDER BY node built on a btree
(below). What it would take, so that the step is not re-derived:* `amcanorder` on 18+ only, where
the planner maps an index's `<` through `amtranslatecmptype` (strategies 6..9 to COMPARE_LT..GT, 1
to COMPARE_EQ; scalar families only) - on 16 and 17 core looks for btree strategy 1, which is
lion's `=`, and so could never see an order; a `get_relation_info_hook` that trims `sortopfamily`
for a column whose RECORDED order is hash order (§21, "The order is the index's"), for multi-key
columns, for multicolumn indexes (lion orders one column at a time) and for DESC/NULLS FIRST
indoptions, which `amcanorder` makes CREATE INDEX accept; NULLs returned last (the reserved NULL
entry sorts first, §21); IN lists walked entry by entry in directory order instead of the union of
§29.3, because the AM is never told whether the plan needs the order (the executor passes only a
direction), so an ordered AM has to produce it always; backward paths, which indxpath.c builds for
every ordered index regardless of `amcanbackward`, either served by a descending walk (§21's
leftlink) or pruned; and ordered-scan costing with a startup cost that lets LIMIT stop early.
Everything else - the walk, the per-entry stream, the batch, the pin rules - would carry over
unchanged, since a WALK is already in key order.

**What the btree-ordered node planned next can use.** That node walks a btree on the ORDER BY
column and tests each heap TID against lion's exact answer to the WHERE clause. The source above is
that answer: `LionSource` opened on the WHERE clause's scan keys in the SETS shape is one
ascending, duplicate-free stream of containers (container keys strictly increasing), so "build
once, then `contains(tid)`" is a copy of the containers the stream produces into an array - memory
equal to the answer's container bytes - searched by container key and tested with
`lion_container_contains()`; or, if that is too large, a TIDBitmap filled from the same stream. A
WALK source (a range) streams entry by entry, so its union has to be accumulated (a TIDBitmap, or
container keys ORed as §15 does) before it can be probed. `lion_source_exact()` says whether the
set is exact (§29.6). The entry points are `lion_source_open()`, `lion_source_next()` and
`lion_source_close()` in lion_scan.c, declared in lion_count.h; they take an index, scan keys and
the `keeppins` switch of §29.5, and no IndexScanDesc. §30 is that node (`LionOrdered`): it copies
the containers of one source per lion leaf of core's bitmap qual tree into a sorted array, sorts
and merges a WALK's or a LIST's once they are complete, and ANDs and ORs the leaves' sets.

### 29.9 Index-only scans (step 3), for later - and the one kind that exists already

**An index-only scan of a query that needs no column exists already** (found while implementing
step 1: `count.sql` failed with core's `no data returned for index-only scan`). check_index_only()
allows an index-only scan when every column the query needs can be returned, and a query that
needs none - `SELECT count(*) FROM t`, `count(*) WHERE <a partial index's predicate>` - needs
none; `amoptionalkey` lets the planner scan a lion index with no key at all, and once
`amgettuple` exists it builds that path and sometimes picks it. It is served, not refused:

- the executor reads the tuple from `xs_itup` (and its shape from `xs_itupdesc`, which
  `ambeginscan` now sets), so the scan hands it a tuple of NULLs of the index's own shape, formed
  once; nothing reads it, because there is no column in the target list or the quals to read
  (the recheck qual is empty too: a key would put its column among the needed ones);
- `xs_want_itup` turns `dropPin` off, so a WALK keeps the directory leaf of each INLINE entry and
  the posting leaf of each container pinned until the next batch - the §9 interlock, which is
  what makes the executor's visibility-map test of each returned TID safe, exactly as for the
  count;
- the UNION shape (a multi-key first column) returns only TIDs the index holds (§29.6) but pins
  none of the pages they came from, so each of them is looked up in the heap under the scan's
  snapshot first (`lion_table_fetch_tid()`) and only a visible one is handed on - which stays
  visible to that snapshot, so nothing can take it away before the executor's own test. A visible
  tuple at a TID the index holds is a row of the index: a HOT chain cannot change a column the
  predicate reads. Slow and exact; the count pushdown answers the common shapes instead anyway.

What a real `amcanreturn` would need from this scan:

- **A value to return.** A lion column's stored key is the value only under the §10
  value-representation contract (`BTEQUALIMAGE_PROC`, the bpchar allowlist): citext's two
  spellings share an entry, so the stored key is not every row's value. `amcanreturn` is per
  column and has to apply that contract; multi-key columns can never return (their keys are
  extracted, not the column). The value comes from the ENTRY, not from a TID, so the scan sets
  `xs_itup` (or `xs_hitup`) once per entry and returns it with every TID of that entry - which
  needs the WALK shape or a per-entry list, since a SETS union by container key does not know
  which entry a TID came from. A single-key equality knows it trivially.
- **The §9 interlock.** An index-only scan skips the heap for all-visible pages, which is exactly
  the count's situation: the pin on the page a container came from has to be held until the
  executor has checked the visibility map for that container's TIDs. `xs_want_itup` therefore turns
  `dropPin` off (btree does the same, nbtree README above), and the batch rule of §29.5 already
  holds the pin until the next batch is pulled - i.e. until every TID of the current container has
  been returned and its VM bit tested. Nothing else changes; a NOPIN set then has to be relocated
  under a pin (`lion_posting_set_relocate()`, as the count does) rather than rechecked.
- **Costing**: cost_index() applies the all-visible fraction itself once `canreturn` is set.

### 29.10 Backward scans and more (step 4, later)

A descending walk (directory leaves by leftlink, which §21 keeps and verify() checks; posting
leaves have none, so a descending container order needs the tree's pivots or a reversed batch),
`(a, b)` order on multicolumn indexes, and - if ever wanted on 16/17 - an ordered CustomScan.

### 29.11 Cost (`lioncostestimate()`)

Plain and bitmap index paths are the same IndexPath and share `amcostestimate`; the planner prices
the heap side of each (cost_index() for a plain scan, cost_bitmap_heap_scan() for a bitmap one).
The only input that differs is `indexCorrelation`, which only cost_index() reads, and lion used to
return 0 for it. It now returns what btcostestimate() returns: the ANALYZE correlation of the
column whose qual the scan answers (`STATISTIC_KIND_CORRELATION` for the type's default `<`, the
operator ANALYZE computes it with), times 0.75 for a multicolumn index as btree does; 0 for a
multi-key column and whenever no statistic exists. That is the right number for the same reason it
is for btree: within one key a lion scan returns TIDs in heap order (as btree does since its
heap-TID tiebreaker), so how the matching rows are spread over the heap is what the column's
correlation describes - a column stored in key order reads its rows sequentially, a scattered one
pays a random page per row. The index side is unchanged. The effect on the planner's choices is
btree's: one row goes to the plain scan (no bitmap to build, no bitmap heap overhead), a few
thousand scattered rows go to the bitmap at a normal `work_mem` (sorted page visits), and at a
`work_mem` so low that the bitmap is priced lossy - every tuple of every lossy page rechecked,
compute_bitmap_pages() - the plain scan wins again. The count pushdown competes at the upper rel
with its own cost (§10); the regression suite pins its choices.

**The count's recheck is priced the same way** (2026-09-24 review). With a correlation, a plain
scan of a clustered value is cheap: 5000 rows of one value on 32 heap pages. The count pushdown
charged its heap recheck one page per candidate TID up to the dirty part of the heap, so when the
visibility map pg_class remembers had gone stale - `relallvisible` is only refreshed by VACUUM,
while updates, and on 19+ on-access pruning, change the map - it priced the same count at 5036
against the scan's 3252 and lost, running 2.4-2.8x slower (0.6 ms against 0.24). The recheck
visits each page holding a candidate once, in block order, so for a count whose WHERE is one
plain equality the pages are now what cost_index() would say they are: one per row when the values
are scattered, the rows' share of the heap when they are stored in order, interpolated by the
correlation's square (`lion_cost_count_rel()`, `lion_single_eq_var()`). Only ever lower, and only
for that shape; a range already had the rule (§28), and other WHERE shapes keep the old bound.

### 29.12 Tests

`test/sql/indexscan.sql`, written before the code and failing on HEAD (no plan can show an Index
Scan using a lion index there): every answer compared with a sequential scan's as a multiset;
plans with nothing disabled for a one-row lookup and for low `work_mem`, and every predicate kind
forced through a plain Index Scan - equality, IN and `= ANY` with NULL elements, ranges, `IS NULL`
and `IS NOT NULL`, multicolumn ANDs, arrays and tsvector with recheck, cross-type keys, citext, a
collation mismatch declined; nested-loop inner scans with rescans; cursors (FETCH forward, SCROLL
over a Material, NO SCROLL refusing backward); a dirty heap after updates and deletes, then VACUUM;
a posting set far larger than one batch with its memory flat; an exclusion constraint; the count
pushdown still chosen for its shapes, a clustered value under a stale visibility map included; a
partial multi-key index read whole at 64 kB of work_mem, by a plain scan and by index-only scans
with nothing disabled, clean and dirty (the review's repros: rows the index does not hold came
back); a 20,000-value Param list read through a cursor within work_mem, and long lists with
duplicates, NULLs, citext spellings, a range and another column beside them, at 32 values a batch; index-only scans of no-column queries (a column's walk, a
partial index, a multi-key column's bitmap), clean and dirty. Existing tests whose helpers exist to
exercise the BITMAP path, or to force the count pushdown by disabling every other scan, now disable
plain index scans as well; the plan pins that changed are one-row multi-key lookups, which are now
plain Index Scans. Isolation: `gettuple_pause.spec` (cursors - the pause the executor really makes,
between two `amgettuple` calls - so it runs on every major) parks a scan after five rows while the
posting leaves under it split, the INLINE set it copied spills, the directory leaf under a range
walk splits, and VACUUM deletes the NEXT entry of the walk and frees its posting tree, whose pages
an insert then takes back; each drains to exactly the rows its snapshot sees, none twice, and
pg_buffercache shows the paused scan pinning no index page, so the VACUUM completes under it.
`gettuple_dirty_pin.spec` (injection point `lion-gettuple-batch`, 17+) parks an exclusion-
constraint check with its batch loaded and shows VACUUM waiting for its pin, and a plain MVCC scan
parked at the same point holding none while the same VACUUM completes.

### 29.13 Measured (2026-09-24, the prune slot's PostgreSQL 20devel, assert-enabled: ratios, not absolute numbers)

One million rows of the benchmark's scalar table (`bench/comprehensive/workloads.py`,
`scalar_data`), twice: one copy with lion indexes on `c200`, `c20k` and `c1m`, one with btree
indexes on the same columns, both vacuumed and warm. The queries are the benchmark's heap fetches
(`sum(id), sum(length(payload))`, so neither index can skip the heap), count pushdown off; median
of 21 runs (9 for `fetch_medium`) of `EXPLAIN (ANALYZE, TIMING OFF)`'s execution time, in ms.
"default" is the plan with nothing disabled; the other columns force one path each.

| query | work_mem | lion default | lion Index Scan | lion bitmap | btree Index Scan | btree default |
|---|---|---|---|---|---|---|
| `fetch_c1m` (`c1m = 12345`, 1 row) | 64MB | Index Scan | 0.023 | 0.025 | 0.021 | Index Scan 0.021 |
| `fetch_rare` (`c20k = 123`, ~50 rows) | 64MB | bitmap 0.058 | 0.053 | 0.057 | 0.046 | bitmap 0.058 |
| `fetch_medium` (`c200 = 17`, 5000 rows) | 64MB | bitmap 5.18 | 3.45 | 5.04 | 3.55 | bitmap 5.08 |
| `fetch_c1m` | 64kB | Index Scan 0.023 | 0.023 | 0.025 | 0.021 | Index Scan 0.020 |
| `fetch_rare` | 64kB | bitmap 0.058 | 0.052 | 0.058 | 0.046 | bitmap 0.059 |
| `fetch_medium` | 64kB | Index Scan 3.62 | 3.44 | **19.3 (lossy)** | 3.40 | Index Scan 3.40 |

The plain lion scan is within 10-15% of btree's on one and fifty rows and level with it on five
thousand, and the planner makes the same choices for both indexes in every cell: the plain scan for
one row, the bitmap at a normal `work_mem` (which for these warm, in-memory runs is the slower of
the two for both AMs, so the choice is btree's cost model's, not lion's), and the plain scan at 64 kB,
where the lion bitmap goes lossy and costs 5.6x. The count pushdown keeps every shape of the
benchmark's scalar cases at 200k rows (checked by comparing each case's plan with plain index scans
enabled and disabled: only the two heap-fetch cases above that a plain scan now serves changed).

## 30. Lion-filtered, btree-ordered scans (`LionOrdered`, lion_ordered.c)

The shape is

    SELECT ... FROM t WHERE <clauses a lion index answers> [AND <anything else>]
    ORDER BY <columns an ordered index on t provides> [LIMIT n [OFFSET m]]

and the two plans core can make for it, when the WHERE is selective and the ORDER BY is not on the
lion-indexed columns, are each wrong in their own way:

1. walk the ordered index (a btree on the ORDER BY column), fetch EVERY heap row it names and
   filter it - with a 0.25% filter, 400 heap fetches per row returned;
2. a lion bitmap heap scan and a top-N Sort - which fetches every matching row, 2,500 of them per
   million, to return ten.

Core cannot filter with one index and order with another. `LionOrdered` does: it builds lion's
EXACT answer to the WHERE clauses lion can answer, once, as a set of TIDs in memory; walks the
btree in order reading only index tuples; tests each TID for membership BEFORE it touches the
heap; and fetches, rechecks and returns only the members, until the LIMIT above stops pulling. Its
cost is the lion lookups, the btree entries walked (index pages only) and one heap fetch per row
returned (plus the members the rest of the WHERE rejects). Because the order comes from the btree,
the node works on every supported major (16 .. 20), and DESC, multi-column ORDER BY, NULLS
FIRST/LAST, expression indexes and the btree's own index quals come with it for free. This
replaces native ordered lion scans (§29.8, `amcanorder` on 18+), which stay deferred.

### 30.1 What qualifies

- **The relation.** A plain base relation (`RELOPT_BASEREL`, `RTE_RELATION`, relkind table or
  materialized view, not an inheritance parent, no TABLESAMPLE) of the heap table AM
  (`lion_table_am_supported()`, §2), with at least one lion index and one ordered index. Declined:
  a relation with security quals - RLS policies or a security-barrier view, i.e. an RTE with
  `securityQuals` or any restriction clause with `security_level > 0` - exactly as LionCount
  declines them (§9, "Row-level security"): the ordinary plan applies them with core's
  leakproofness rules. The target relation of an UPDATE, DELETE or MERGE is declined, and so are
  partitioned tables and their partitions, in v1 (§30.8).
- **The WHERE.** The relation's restriction clauses, an implicit AND. Some of them are answered by
  lion indexes and the rest stay a heap-side filter. WHICH of them lion answers, and how, is not
  decided here but by core's own index matching: every shape the bitmap path supports - `=`, IN /
  `= ANY`, the ranges of §28, `IS [NOT] NULL`, multicolumn indexes (§24), arrays and tsvector
  (§17), a partial index whose predicate the query implies, an OR across lion-indexed columns (a
  BitmapOr), several indexes ANDed (a BitmapAnd), Params wherever a Const may stand - is accepted,
  because the node's lion side IS a bitmap path's `bitmapqual` tree (§30.2).
- **The ORDER BY.** Any pathkeys an ordered index scan of the relation provides: core's own
  ordered `IndexPath`s, taken from the relation's path list, so that btree ordering (direction,
  NULLS placement, multi-column prefixes, expression indexes, opclass orderings, the btree's own
  index quals) is never re-implemented. An index scanned with ORDER BY operators (KNN) is not an
  ordered path in this sense and is skipped, and a lion index has no order (§29.1). In core this
  means a btree.
- **LIMIT** is optional: the node is priced like any path, with a start-up and a total cost
  (§30.3), and core's LIMIT and sort planning choose. Without a LIMIT it can still beat a Sort for a
  selective result, and loses for a large one; the cost decides.

### 30.2 Planner integration

`set_rel_pathlist_hook`, chained to any previous hook and calling it FIRST, as the upper-paths
hook does (§23, "hook coexistence"). GUC `pg_lion.enable_ordered_scan` (bool, default on). For a
qualifying relation the hook runs after core has built the relation's paths, and:

1. **The ordered side.** Every unparameterized `IndexPath` in `rel->pathlist` - an Index Scan or
   an Index Only Scan path: the node visits the heap anyway, so the same index serves - with
   non-NIL pathkeys, no ORDER BY operators, a BTREE (§30.4's early stop and switch rest on a btree
   returning each heap TID once per scan, in its pathkeys' order; any other ordered AM is declined,
   2026-09-25 review), and no `RowCompareExpr` among its index clauses (v1). Core builds these exactly when the pathkeys are
   useful to the query (its ORDER BY, or a merge join), and keeps each unless a cheaper path with
   the same order exists.
2. **The lion side, found by core.** `create_index_paths()` is run once more, on a SCRATCH copy of
   the RelOptInfo whose `indexlist` holds only the lion indexes, whose path lists start empty, and
   with its join clauses, eclass joins and parallelism switched off - so that only
   unparameterized, non-partial paths are built and the real rel is left untouched. What it
   builds - bitmap heap paths, whose `bitmapqual` is a lion IndexPath or a BitmapAnd/BitmapOr tree
   over lion IndexPaths, and plain lion index paths, each a one-leaf `bitmapqual` - are the
   candidate lion accesses. Only core's matching decides what a lion index can answer (operator
   families, collations, partial-index predicates, OR arms), so the node accepts exactly what a
   lion bitmap scan would.
3. **One CustomPath per (ordered path, lion access)** pair, each offered to `add_path()`, which
   keeps whichever of them is not dominated in start-up or total cost at those pathkeys. The path
   carries the ordered path's `pathkeys`, `rows = rel->rows`, `param_info = NULL`,
   `pathtarget = rel->reltarget`, `parallel_safe = false` and
   `flags = CUSTOMPATH_SUPPORT_PROJECTION`: no backward scan (a SCROLL cursor gets a Material),
   no mark/restore (a merge join restores through a Material).
4. **Which clauses the node still evaluates** is decided the way `create_bitmap_scan_plan()`
   decides it. The lion side's ORIGINAL qual, `lionqual`: the clauses of the leaves' index
   clauses, plus each partial index's predicate where they do not imply it, ANDed across a
   BitmapAnd and ORed across a BitmapOr - `create_bitmap_subplan()`'s `qual`. A restriction clause
   leaves the heap filter when it is (by pointer) one of the ordered path's non-lossy index
   clauses, or one of the index clauses of a lion leaf reached from the root through ANDS ONLY,
   or when `predicate_implied_by()` the `lionqual`; the rest is the plan's `qual`, the filter
   core's `ExecScan()` applies. Pseudo-constant clauses are core's gating Result's.
   *(Fixed 2026-09-25: the first version dropped every leaf's clauses, OR arms' included. Core
   builds each arm of a BitmapOr with the other top-level clauses at hand, so for `a = 3 AND
   (b = 5 OR c = 7)` over a lion index on `(a, b)` the arm `a = 3 AND b = 5` reuses the top-level
   RestrictInfo of `a = 3`; the lion qual `((a = 3 AND b = 5) OR c = 7)` does not imply it, yet it
   left the filter, and at default settings 38 of 50 rows had `a <> 3`. An arm's clauses are now
   never treated as implied; only what `predicate_implied_by()` proves of the whole lion qual, or
   an AND-path leaf's own clause, leaves the filter - which is what `create_bitmap_scan_plan()`
   does. Nothing else in the node assumes a leaf's clause is implied: an exact set is exact for
   the `lionqual` as a whole, and the recheck of an inexact one is the whole `lionqual`.)*

The plan carries in `custom_exprs` - where setrefs.c fixes their Vars and Params, and where
`SS_finalize_plan()` finds the Param ids that make the executor rescan the node - the lion leaves'
index quals (key on the left, as `IndexClause.indexquals` gives them), the ordered index's index
quals, the `lionqual` and the ordered index's original clauses; and in a positional
`custom_private` behind a shape marker (§10) the ordered index, its scan direction, the lion tree
(AND / OR / leaf, preorder) with each leaf's index and index columns, and whether a lion index
clause was lossy. The executor substitutes an `INDEX_VAR` Var for each index qual's key operand,
as `fix_indexqual_references()` would, and builds the scan keys with core's
`ExecIndexBuildScanKeys()`, so a Param becomes a run-time key exactly as in an Index Scan.

No overlap with LionCount (§10) or the FK join (§27): those replace an aggregate at the upper rel;
this is a scan path of the base rel.

### 30.3 Cost

With `T = rel->tuples`, the lion access's selectivity `s` and index cost `C_lion` (core's
`cost_bitmap_tree_node()`: the leaves' `indextotalcost`s and the BitmapAnd/Or overhead), and the
ordered path's selectivity `s_o` (the fraction of the index its own quals leave) and index cost
`C_ord` (its `indextotalcost`: the whole walk, index pages and index tuples):

- **start-up** = `C_lion` + copying the answer into the set, `containers x cpu_operator_cost`,
  with `containers = min(heap pages / LION_BLOCKS_PER_CONTAINER, s T)`;
- **the walk** = `C_ord` + `s_o T` membership tests at `cpu_operator_cost` each;
- **the heap** = `F = s_o T s` members fetched, priced as `cost_index()` prices heap fetches:
  `index_pages_fetched()` at `random_page_cost` for an uncorrelated order, the members' share of
  the heap for a correlated one, interpolated by the square of the ordered index's correlation
  (asked of its own `amcostestimate`); plus `cpu_tuple_cost` and the filter's per-tuple cost for
  each of the `F`, and the target's cost per output row;
- **total** = start-up + walk + heap; rows = `rel->rows`.

Core's LIMIT planning scales a path's run cost by the fraction of its rows the LIMIT takes
(`adjust_limit_rows_costs()`), which is exactly how the node behaves: the walk and the fetches stop
when the LIMIT stops pulling, and the set is built in full before the first row. So `LIMIT 10` of a
0.25% filter pays the lion lookups, 4,000 btree entries and 10 heap fetches, where core's ordered
walk pays 4,000 heap fetches and bitmap + Sort all 2,500 matches of a million rows. For an
UNSELECTIVE filter (`c2 = 1`, half the rows) the ordered walk needs 20 entries for 10 rows and no
start-up at all, while the node first reads half a million TIDs out of lion: core wins, and the
model says so. Without a LIMIT and with a large result the node walks the whole btree and fetches
the rows in index order, i.e. randomly, while the bitmap scan fetches them in heap order and a
Sort is cheaper: the model says that too.

**The hazard: a filter correlated with the order** (2026-09-25 review). LIMIT scaling assumes the
members are spread evenly along the btree, so that `LIMIT 10` of a 0.25% filter meets its rows
after 4,000 entries. When the filter is correlated with the order - `c = 398 ORDER BY k` where
`c` is `k / 2500`, or a zero-row AND of two correlated columns - the members lie at the far end
(or nowhere), and the walk reads almost the whole index: 997,510 entries and 41.5 ms on 1M rows,
against 0.6 ms for bitmap + Sort. Core's own ordered walk has the same hazard (it paid 113 ms
there), but the node competes with bitmap + Sort as well, and the planner cannot see the
correlation (no cross-column or filter-to-order statistics). The model is left as it is; the
EXECUTOR bounds the damage instead (§30.4, "When the walk is not paying"): the set's exact size
is known before the walk starts, and once the walk has cost what fetching the remaining members
would, it fetches and sorts those. The same query now takes 2.8 ms: 80,000 entries walked, then
2,500 members fetched and sorted (§30.10). Still 3.5x the bitmap plan's time - the cost of not
knowing - but no longer 50x.

**Memory ceiling.** The set is the answer's containers (§3): at most 4 kB per 64 heap blocks and
about 2 bytes per member for a sparse answer, plus 16 bytes of directory per container - some
12 MB for half of 100M rows. It has to fit in `hash_mem` (`get_hash_memory_limit()`, `work_mem x
hash_mem_multiplier`, what core allows one node's in-memory hash table), and the planner does not
offer a path whose estimate exceeds it. The estimate can be wrong, so the executor DEGRADES rather
than overruns (§30.4).

### 30.4 Execution

- **BeginCustomScan** opens the ordered index and every lion index (AccessShareLock; the heap and
  the plan's filter are core's `ExecInitCustomScan()`'s), checks the table AM again, builds the
  scan keys (`ExecIndexBuildScanKeys()`; run-time keys get an ExprContext of their own, as an Index
  Scan's do), and initialises two recheck quals with `ExecInitQual()`: the `lionqual` and the
  ordered index's original clauses. The scan slot is a buffer heap tuple slot. A non-MVCC snapshot
  is an internal error: every snapshot a SELECT's executor runs under is MVCC, and §30.5's
  argument needs one. Under SERIALIZABLE it takes `PredicateLockRelation()` on every lion index
  before any lookup (§9, "SERIALIZABLE": the AM has no `ampredlocks`, and the node reads it
  without `index_beginscan()`).
- **The set is built at the first fetch after a start or a rescan**, not in BeginCustomScan: an
  exec Param is set by a nested loop after the node is initialised. Each lion leaf evaluates its
  run-time keys, opens `lion_source_open(index, keys, nkeys, keeppins = false, cxt)` (§29.3,
  §29.8; an MVCC reader holds no pin, §29.5), and copies every container it produces into a
  `LionTidSet`: a sorted array of (container key, container copy), searched by binary search and
  tested with `lion_container_contains()`. A SETS or UNION source arrives in ascending container
  key (`lion_source_sorted()`); a WALK (a range) or a LIST (a long IN list) arrives entry by entry
  or batch by batch and is sorted, with equal keys ORed, once it is complete. A BitmapAnd is the
  container-wise AND of its children's sets, a BitmapOr their OR; a leaf that selects nothing is an
  empty set. No lossy page exists anywhere in this: every member is a TID some entry held (§29.6).
- **Exact, or rechecked.** The set is EXACT when every leaf's `lion_source_exact()` is true, no
  lion index clause was lossy and the set did not degrade. Then every member satisfies the
  `lionqual` and nothing is rechecked; otherwise every fetched member is tested against the
  `lionqual` (the multi-key strategies of §17, a second qual on one column, §29.6), and one that
  fails counts as removed by the lion recheck.
- **Degrading.** The set counts its bytes; when they would exceed `get_hash_memory_limit()` it
  frees every container and keeps only their KEYS - "every TID of these 64 heap blocks may be a
  member" -, marks itself inexact, and carries on that way. Memory is then 16 bytes per container
  key, at most one per 64 heap blocks; the answer stays exact because every fetched member is now
  rechecked; the node walks on as a slower filter.
- **The walk.** `index_beginscan()` on the ordered index under the executor snapshot and
  `index_rescan()` with its keys, in the path's direction. On 16 .. 19 each TID comes from
  `index_getnext_tid()`, which reads only the index, and a member is fetched with
  `index_fetch_heap()`, which follows the HOT chain under the snapshot and lets the btree mark
  entries whose chains are dead to all, as an ordinary index scan does. PostgreSQL 20 moved the
  heap side of index scans into the table AM (`table_index_getnext_slot()`; there is no
  `index_getnext_tid()` or `index_fetch_heap()`), so there the node takes the TID from the index
  AM through `tableam_index_getnext_tid()` and fetches a member with `table_fetch_tid()` (the HOT
  chain under the snapshot, the TID moved to the version it sees) and
  `table_tuple_fetch_row_version()`. A member with no visible version is skipped; a visible one is
  rechecked when the set is inexact (above) or the index set `xs_recheck` (against the ordered
  index's original clauses), then handed to `ExecScan()`, which applies the filter and projects.
  A set with nothing in it ends the scan before the walk starts.
- **Stopping early.** Once the walk has met as many DISTINCT members as the set holds (its
  cardinality, known unless it degraded) it stops: nothing further along can be one. The walk
  remembers which members it has met - one 4 kB bitmap per container key it touches, in a per-scan
  context, within `hash_mem` (past that it stops tracking and simply walks on, neither stopping
  early nor switching) - and a member counts once however often the walk meets its TID.
  *(Fixed 2026-09-25: the first version counted every meeting, on the premise that a btree
  returns each TID once. Within one scan it returns each INDEX TUPLE once, but under an MVCC
  snapshot the paused walk holds no pin (nbtree's `dropPin`, and lion's §29.5), so while a
  cursor waits VACUUM can remove a dead member X the walk has already passed and an insert can
  take X's slot with a key further along; the walk then meets X's TID again, as a tuple the
  snapshot cannot see. Counted twice, it made the count reach the cardinality one visible member
  early, and `FETCH ALL` stopped before B: test/isolation/ordered_recycle.spec. The choice between
  the two fixes the review offered: remembering the members met, or counting only members fetched
  VISIBLE (a recycled slot is never visible to the snapshot, so that count cannot overshoot).
  The second needs no memory but, on a table with dead members, rarely stops early at all; the
  first stops exactly when the last member has been met, dead or alive, and the switch below
  needs the same record anyway. A second meeting of a TID is skipped without a fetch, which is
  right: the tuple it now names was written after the snapshot. The premise that the ordered
  index hands out each index tuple once is why the node is limited to btree.)*
- **When the walk is not paying: fetch and sort.** The set's exact size is known before the walk
  starts, so the walk is a bet on reaching `LIMIT` rows early, and the executor can cap the bet.
  Once this scan has walked at least `LO_SWITCH_MIN_WALK` (10,000) entries and at least
  `LO_SWITCH_RATIO` (32) entries per member it has not met yet, it stops walking, fetches every
  member it has not met in TID order (the HOT chain under the snapshot, then the version it
  sees), keeps the visible ones that pass the lion recheck (an inexact set) and the ordered
  index's own original clauses (these rows did not come through the index), sorts them by the
  path's pathkeys (SortSupport on the pathkeys' own sort operators, collations and NULLS
  placement) and returns them. This is the ski-rental rule: the ratio is what a member's heap
  fetch costs in index entries (0.04 us an entry against 1-2 us a random fetch, §30.10), so by
  the time the node switches it has spent on the walk about what the fetches cost, and in the
  worst case pays about twice what fetching and sorting from the start would have; the 10,000
  floor keeps a small set's walk, which costs well under a millisecond, from switching at all.
  Correctness: every row the walk has not returned yet is among the members it has not met (a
  visible member's index entry lies where the walk has not been, and the walk only skips
  non-members), and none it has returned is (those were met); rows with equal sort keys may come
  in any order, which is all the pathkeys promise. The switch needs the pathkeys to be plain
  columns of the relation - an expression index's order would evaluate its functions where the
  ordinary plan calls none, which a revoked EXECUTE could tell apart (§30.6) - and gives up,
  letting the walk go on, if the rows do not fit in `work_mem`. EXPLAIN ANALYZE says
  `Switched to Fetch and Sort: S of N scans, M members fetched`. A rescan starts afresh.
- **EvalPlanQual** (`SELECT ... FOR UPDATE` over the node): the recheck method tests the
  substituted row against the `lionqual` and the ordered index's original clauses, as an Index
  Scan's `IndexRecheck()` does; `ExecScan()` applies the filter.
- **Rescans.** `ReScanCustomScan` restarts the walk (its keys are evaluated again at the next
  fetch) and rebuilds the set only when a Param of the lion quals changed (`chgParam` against
  `pull_paramids()` of the lion quals, taken at BeginCustomScan). A LATERAL `ORDER BY ... LIMIT`
  subquery whose lion filter takes the outer row's value rebuilds per outer row; one whose Param
  is only in the btree's quals keeps its set. A rescan with no changed Param (a cursor rewound)
  keeps it too: the snapshot is the same.
- **EndCustomScan** ends the index scan, closes the indexes and frees the node's memory.

### 30.5 Correctness and concurrency

Let S be the executor snapshot (MVCC), taken before the node starts; the set M is built from the
lion indexes at a time t1 after S was taken; the btree is walked afterwards. A row is returned iff
its TID comes out of the btree walk, is in M, has a version visible to S at the fetch, and that
version passes the rechecks and the filter. The claim is that these are exactly the rows the
ordinary plan returns, in the same order.

- **A row visible to S that satisfies the lion quals is in M.** Its inserting transaction committed
  before S was taken (or is ours, at an earlier command id); an index entry is written before its
  transaction commits, so lion held its TID before S and therefore at t1. Only VACUUM removes it,
  and only once the row is dead to every snapshot, S included.
- **HOT chains.** A HOT update changes no column of any index that is not summarizing
  (`HeapDetermineColumnsInfo()` against the relation's HOT-blocking columns; lion sets
  `amsummarizing = false`), so every version of a chain has the same lion-indexed values and the
  same btree-indexed values. Both indexes hold only the ROOT TID of a chain: the btree returns the
  root, M holds the root, and `index_fetch_heap()` / `table_fetch_tid()` find the version S sees
  from it. An update of a lion-indexed column or of the btree column is never HOT - it inserts new
  entries into both indexes under the new version's TID - so "the root is in M" means "the version
  S sees satisfies the lion quals" (up to the recheck of an inexact set).
- **A TID in M whose row S cannot see** - dead, deleted by a transaction committed before S, or
  inserted by one S does not see, before or after t1 - is filtered by the heap visibility check,
  the check every Index Scan relies on. Rows inserted after t1 are not in M, and they cannot be
  visible to S (their transaction was running or not started when S was taken), so missing them is
  right. Rows deleted after S was taken are still visible to S and still in the lion index (VACUUM
  cannot remove them while S holds the horizon back), so they are in M and returned, as the
  ordinary plan returns them.
- **A recycled TID.** M is private memory and nothing pins the lion pages it came from (§29.5's
  MVCC rule). If VACUUM removes a member's tuple after t1 and its slot is reused, the new tuple was
  written after t1 by a transaction S cannot see: the fetch finds nothing visible. The btree side is
  an ordinary index scan under core's own interlock (nbtree's MVCC `dropPin` rule).
- **Order.** Rows come out in the order the btree walk returns them, which is what the pathkeys
  claim; a non-member is only ever skipped, never reordered. After a fetch-and-sort switch the
  rest come out sorted by the pathkeys, all of them at or after the last row the walk returned
  (§30.4).
- **Non-MVCC snapshots** are refused (§30.4). **Hot standby**: nothing here reads the visibility
  map or relies on a pin, so a standby needs nothing an Index Scan does not.
- **SERIALIZABLE.** The btree walk takes its own page predicate locks; each heap fetch takes a
  tuple lock (`heap_hot_search_buffer()`); and the node takes a relation predicate lock on every
  lion index it builds M from. A concurrent write that would change the answer inserts into a lion
  index (a new matching row, or a non-HOT update that makes a skipped row match: a conflict with
  the relation lock), or writes a row the node returned (its tuple lock), or inserts into a btree
  page the walk read. A skipped non-member that is deleted, or updated and still does not match,
  does not change the answer; the ordinary plan's tuple lock on it is a false positive the node
  does not reproduce. test/isolation/ordered_serializable.spec is the write-skew case.

### 30.6 Security

- **Privileges** are the range table's: the node scans the relation (scanrelid > 0), and
  `ExecCheckPermissions()` checks it like any scan.
- **EXECUTE** (§9, "EXECUTE on what a count replaces"): the node evaluates the plan's filter
  (initialised by core), the `lionqual` and the ordered index's original clauses, all through
  `ExecInitQual()`, which checks EXECUTE on every function and operator for the current user at
  executor start - the checks the ordinary plans make (an Index Scan initialises its
  `indexqualorig` and its filter, a bitmap heap scan its `bitmapqualorig`). The `lionqual` is
  initialised even when the set is exact, as `indexqualorig` is. Neither ordinary plan checks the
  btree's ordering comparison, and neither does the node. test/sql/ordered.sql revokes an equality
  function from PUBLIC and gets core's error from both plans.
- **RLS and security-barrier views** are declined (§30.1). **Leakproofness** does not arise beyond
  that: the membership test is not user code, and the user's quals run on a row only after the
  heap visibility check, as in every core scan.

### 30.7 EXPLAIN

    Limit
      ->  Custom Scan (LionOrdered) on fact
            Filter: (length(payload) > 60)           -- core's, printed first by core
            Ordered By: fact_c1m_idx (backward)      -- "(backward)" for a backward walk
            Index Cond: (c1m > 100)                  -- the ordered index's own quals, if any
            Lion Cond: ((c200 = 17) AND (c2 = 1))    -- the lionqual
            Lion Indexes: fact_c200_idx, fact_c2_idx

and with ANALYZE `Index Entries Walked`, `Lion Set Hits` (walked entries that were members),
`Heap Fetches` (members with a visible version), `Rows Removed by Lion Recheck` (printed when
the set is not exact or something was removed), and `Lion Set: N containers, exact | rechecked |
degraded[, B builds]`, the builds counted when rescans rebuilt it (a LATERAL subquery whose lion
filter takes the outer row's value: one per outer row).

### 30.8 Declined in v1, and why

- **Partitioned tables.** A partitioned parent's ordered plan is a MergeAppend over per-partition
  ordered paths, and the hook also runs for each partition (`RELOPT_OTHER_MEMBER_REL`), so
  offering the path there might just work; but run-time pruning, a partition's translated security
  quals and the pathkeys of child rels are a surface of their own to test. The natural v2.
- **The target relation of an UPDATE, DELETE or MERGE**, which a merge join's ordered input could
  otherwise make the node scan: its row identity and EvalPlanQual rechecks would work as they do
  for `SELECT ... FOR UPDATE` (tested), but nothing needs it and nothing tests it.
- **Parameterized paths** (the node as the inner side of a nested loop with `t.x = outer.y` pushed
  into it): an inner side needs no order, so an ordered filter scan has nothing to offer there.
  Params inside a subquery (LATERAL, correlated) are ordinary Params and ARE supported (§30.4).
- **Parallel scans** (`parallel_safe = false`), **backward scans**, **mark/restore**: the shape
  needs none of them; core adds a Material where a caller does.
- **RowCompareExpr index quals** on the ordered index (`(a, b) > (1, 2)`): the executor's key
  substitution handles one key column per clause, and such a path is skipped.
- **Ordered indexes other than btree**: §30.4's early stop and switch rest on btree's behaviour.
- **The fetch-and-sort switch for an expression order** (`ORDER BY k % 1000` over an expression
  index): the walk never switches there (§30.4).
- **Lion accesses core did not build** (for example ANDing in a lion index that core's
  `choose_bitmap_and()` judged not worth its heap savings): the node only considers the accesses
  core's own lion index paths contain. A sharper set would save btree steps but no heap fetch.
- **Non-MVCC snapshots, RLS, other table AMs**: above.

### 30.9 Tests

`test/sql/ordered.sql`, written before the node and failing without it (no plan contains
`LionOrdered`): every query compared with the ordinary plan (`pg_lion.enable_ordered_scan = off`)
row by row IN ORDER - ORDER BY ends in a unique key, so that the order is determined - with its plan
checked to contain the node: `ORDER BY k [DESC] [NULLS FIRST] LIMIT n` under filters of each kind
(=, IN, a long IN list, a range, `IS NULL`, a multicolumn lion index, an OR across two lion indexes,
arrays and tsvector with their recheck, a partial lion index), a multi-column ORDER BY, an
expression index, OFFSET and LIMIT, no LIMIT, a LIMIT beyond the match count, an empty answer, a
filter on the btree column itself, a residual filter, Params under a generic plan, a LATERAL
subquery rescanned with exec Params per outer row, cursors (FETCH forward; a SCROLL cursor over a
Material fetching backward), a dirty heap (updates, deletes, HOT updates) and the same after
VACUUM, a degraded set at a tiny `work_mem`; the declines (RLS, the GUC off, no lion-answerable
clause); EXECUTE revoked (the node and the ordinary plan give the same error, security_exec.sql's
pattern); and the plan choice with nothing disabled - the node for a selective lion filter with
`ORDER BY` a btree column `LIMIT 10`, core's ordered btree scan for an unselective filter, and no
node for a large result without a LIMIT. Isolation: `ordered_cursor.spec` (a cursor paused after
its set was built while another session inserts matching rows, deletes and updates matching rows
and commits: the rest of the cursor returns exactly its snapshot's rows, and rows inserted between
DECLARE and the first FETCH do not appear either; VACUUM runs while it is paused and completes),
`ordered_recycle.spec` (the review's repro of §30.4's recycled TID: `FETCH ALL` returns B) and
`ordered_serializable.spec` (the write-skew pair, both through the node: one of them fails).
Added with the 2026-09-25 fixes, each failing before its fix: `a = 3 AND (b = 5 OR c = 7)` at
default settings and forced (the OR-arm residual; 38 wrong rows of 50), the same shape on the
main fixture, and a filter correlated with the order - members at the far end, a zero-row
correlated AND, three members early and a thousand late (the switch after rows were returned,
including through a paused cursor), NULLs first under DESC with members among the NULLs met
before the switch, a btree qual beside it, and a LATERAL rescan - each compared in order with the
ordinary plan, with the switch shown by EXPLAIN ANALYZE.
`make hookcheck`'s companion module chains `set_rel_pathlist_hook` too and checks, in both load
orders, whether the LionOrdered path is in the rel when the previous hook returns (§23).

### 30.10 Measured (2026-09-24, the vacuum slot's PostgreSQL 20devel, assert-enabled: ratios, not absolute numbers)

One million rows of the benchmark's scalar table (`bench/comprehensive/workloads.py`,
`scalar_data`, the columns the queries touch), lion indexes on `c2`, `c20`, `c200`, `c20k` and
`c1m` and a btree on `c1m` beside them - the `roaring_btree` portfolio of the benchmark -,
vacuumed and warm. The queries are the benchmark's `ordered_*` cases (`SELECT id, payload FROM
fact WHERE ... ORDER BY c1m, id LIMIT 10`; the btree orders by `c1m` and an Incremental Sort above
the scan breaks the ties by `id`). Median of 15 runs of `EXPLAIN (ANALYZE, TIMING OFF)`'s execution
time, in ms; "default" is the plan with nothing disabled, the other columns force one path each
(total cost of the plan in parentheses).

| query | default | LionOrdered | core's ordered btree walk | lion bitmap + Sort |
|---|---|---|---|---|
| `ordered_filter` (`c200 = 17 AND c2 = 1`, 0.25%) | LionOrdered 0.47 | **0.51** (207) | 2.60 (430) | 4.87 (10,944) |
| `ordered_filter_desc` (the same, `DESC`) | LionOrdered 0.55 | **0.61** (207) | 2.54 (430) | 4.41 (10,944) |
| `ordered_broad` (`c2 = 1`, 50%) | btree walk 0.033 | 0.56 (4,442) | **0.032** (2.9) | 191 (38,531) |

The node is 5x faster than the ordered walk and 8-9x faster than the bitmap and Sort for the
selective filter, and the planner picks it; for the unselective one the ordered walk needs 21
entries for 10 rows while the node first copies half a million TIDs (265 containers) out of lion,
17x slower, and the planner keeps the walk. What the node did for `ordered_filter`: 4,636 btree
entries walked, 23 members fetched for 10 rows - core's bitmap qual answers only `c200 = 17` (its
`choose_bitmap_and()` judges `c2 = 1`'s set not worth its heap savings), so `c2 = 1` is the filter
and half the members fail it (§30.8's "lion accesses core did not build"; ANDing `c2`'s set in
would halve the fetches). Without a LIMIT the same query walks 999,767 entries to find all 5,165
members, and the planner keeps the bitmap heap scan and Sort (5.5 ms).

**After the 2026-09-25 fixes** (same slot and build; median of 15). The three benchmark queries
are unchanged within noise - `ordered_filter` 0.72 ms through the node (default plan 0.55),
`ordered_filter_desc` 0.52, `ordered_broad` kept on core's walk (0.032) - because their walks stay
under the switch's 10,000-entry floor. The correlated hazard of §30.3, on a second 1M-row table
with `k = i` (btree) and `c = (i - 1) / 2500`, `g = c % 2` (lion), 0.25% per `c` value:

| query | default | LionOrdered | btree/lion Index Scan + Sort | lion bitmap + Sort |
|---|---|---|---|---|
| `c = 398 ORDER BY k LIMIT 10` (members at the far end) | LionOrdered 2.83 | 2.87 (was ~41) | 0.90 | **0.80** |
| `c = 150 AND g = 1 ORDER BY k LIMIT 10` (no row) | Index Scan + Sort 0.25 | 4.90 | 0.25 | **0.21** |

The first is still chosen by the planner (it cannot see the correlation) and now switches after
80,000 entries, fetching and sorting the 2,500 members: 3.5x the best plan instead of 50x. The
second is not chosen here; forced, it switches after 80,000 entries and fetches 2,500 members that
all fail `g = 1`.
