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
- Multi-column indexes, INCLUDE columns, ordered scans, `amgettuple`, parallel build/scan,
  fine-grained write concurrency (inserts serialize per hash bucket), key sizes above 2000 bytes.

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

Block 0: meta page. Payload struct `LionMetaPageData` { magic 0x52424931, version 3, offset_bits,
container_bits, nbuckets, inline_limit, unused padding to 64 bytes }. Version 2 was the first that
indexes NULL keys (§14); version 3 (§18) added owner_hash/owner_head, which grows the special area
from 16 to 24 bytes and therefore moves every item on every page. An index of an older version is
structurally readable by nothing in this code, so opening one is an ERROR that asks for a REINDEX
rather than a wrong answer.

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

Container pages (per key, CHAIN entries): items are LionContainer structs, ascending ckey within a
page; all ckeys on page P are smaller than all ckeys on P.rightlink. `minckey`/`maxckey` in the
special area are maintained on every change. Page P owns free space like any heap/index page; use
PageAddItemExtended (with explicit offset to keep order), PageIndexTupleOverwrite (handles size
change), PageIndexTupleDeleteNoCompact/PageIndexMultiDelete + PageRepairFragmentation as needed.

Locating the page for a ckey (`lion_chain_find_page`): if the tail is non-empty and ckey ≥ tail.minckey
use tail (the append case; an empty tail has minckey 0 and must not be trusted); otherwise walk from
head and stop at the first non-empty page with maxckey ≥ ckey, or the last page.

Growth (`lion_chain_put_container`): overwrite in place if the page has room; otherwise *split* page P:
move the items at/after the insert position to a freshly allocated page N linked after P; if the
container still does not fit on P, place it alone on a second new page M linked between P and N
(P → M → N → old right). This guarantees progress in one WAL record (P, N, M, entry page = 4 buffers,
the GenericXLog maximum). Items only ever move right and only to pages that are linked immediately
right of the page they came from - which is the property the §9/§11 interlock needs, and it holds
whether the new page was extended onto the end of the relation or recycled out of the free space map
(§18). A whole chain is freed when its entry is deleted (§18); individual pages of a live chain are
never unlinked. `lion_chain_find_page` skips empty pages (VACUUM may leave them mid-chain).
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

Page allocation: `lion_new_buffer_xl()` takes a page - a recycled one from the free space map when
one is safe to take, else a fresh block from ExtendBufferedRel - initialises it and registers it in
the caller's GenericXLog record, so a crash cannot leave an initialised page that nothing links to;
`lion_new_buffer()` is the same in a record of its own. The recycling rule, what `heaprel` is for and
why a chain's head page never comes from the map are in §18. verify() reports DELETED pages as the
ordinary free pages they are, unreferenced never-initialised or empty pages as WARNINGs (the next
VACUUM's sweep turns them into free pages) and anything else as an ERROR.

## 5. Locking protocol (deliberately coarse in v0)

Lock ordering: bucket head page → other bucket pages → container pages (left to right) → new page.
Never lock a page to the left of one you hold. The meta page is read once at relation open and cached.

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
   bucket page, then walk the container chain holding one SHARED page lock at a time: read all items
   and the rightlink under the lock, release, lock the rightlink. Splits only move items to a new
   page immediately to the right and require an EXCLUSIVE lock on the source, so a reader sees every
   container exactly once (it read the items and the rightlink atomically).
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
   CHAIN → walk the chain; each container page is locked with LockBufferForCleanup (this is the
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
    amsupport = 3 (1 = hash function, same as hash AM; 2 and 3 = GIN's extraction procs, §17)
    amoptsprocnum = 0                amcanorder = false        amcanorderbyop = false
    amcanhash = false                amconsistentequality = true   amconsistentordering = false
    amcanbackward = false            amcanunique = false       amcanmulticol = false
    amoptionalkey = false            amsearcharray = true      amsearchnulls = true
    amstorage = true (§17)           amclusterable = false     ampredlocks = false
    amcanparallel = false            amcanbuildparallel = false    amcaninclude = false
    amusemaintenanceworkmem = true   amsummarizing = false     amkeytype = InvalidOid
    amgettuple = NULL                amgetbitmap = liongetbitmap    amcanreturn = NULL
    ammarkpos/amrestrpos = NULL      parallel scan callbacks = NULL
    amcostestimate: genericcostestimate() then indexCorrelation = 0 (as contrib/bloom)
    amoptions: reloptions `buckets` (int, 0 = auto, max 65536; any value, not rounded to a power of
    two), `inline_limit` (int bytes, 64..4096, default 4096) and `max_entries` (int, 0 = unlimited,
    §17) via add_reloption_kind / add_int_reloption / build_reloptions.
    amvalidate: a scalar opclass must have support proc 1 with signature (T) → int4 and operator
    strategy 1; a multi-key one (§17) procs 2 and 3 and strategies within {2,3,4,5}.
    Handler follows contrib/bloom in master: `static const IndexAmRoutine amroutine = {...}` returned
    with PG_RETURN_POINTER.

Operator classes (in `pg_lion--0.1.sql`): one DEFAULT opclass per type, reusing the hash AM's
support-1 functions, plus the two multi-key classes of §17. Generate the list from the dev cluster with
`SELECT ... FROM pg_amproc JOIN pg_opclass ... WHERE amname='hash' AND amprocnum=1` for at least:
int2, int4, int8, oid, bool, "char", text, varchar (via text), bpchar, bytea, uuid, date, time,
timestamp, timestamptz, interval, numeric, float4, float8, macaddr, inet, name, jsonb, enum types
via anyenum (hashenum). Strategy 1 operator = the type's `=`.

## 7. SQL functions (`lion_funcs.c`)

    lion_index_stats(regclass, OUT nbuckets int, OUT bucket_pages bigint, OUT entries bigint,
        OUT inline_entries bigint, OUT container_pages bigint, OUT containers bigint,
        OUT array_containers bigint, OUT bitset_containers bigint, OUT run_containers bigint,
        OUT ntids bigint, OUT container_bytes bigint, OUT free_bytes bigint,
        OUT sparse_segments bigint, OUT sparse_members bigint, OUT null_tids bigint,
        OUT empty_tids bigint, OUT slack_bytes bigint, OUT max_bucket_pages bigint,
        OUT deleted_pages bigint) RETURNS record
        -- container counts include INLINE containers; free_bytes sums bucket and container pages;
        -- null_tids is the member count of the reserved NULL entry (§14) and empty_tids that of
        -- the reserved no-key entry (§17)
    lion_index_verify(regclass, heapallindexed bool DEFAULT false) RETURNS void
        -- ERRORs on any structural inconsistency: page ids/flags, meta values, entry flags,
        -- ascending ckeys within pages and across rightlinks, min/max correctness, container_check
        -- on every container, ntids/ncontainers sums; with heapallindexed, scans the heap with a
        -- fresh snapshot and checks that every visible tuple's TID is present under its key -
        -- refusing (lion_index_usable(), §9) when this transaction's snapshot may not use the index.
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
  table, or SELECT on every indexed column they touch; otherwise `permission denied`. The CustomScan
  path is covered by the executor's own ExecCheckPermissions on the range table.
- **Row-level security.** The SQL functions refuse a table on which RLS applies to the caller
  (policies would have to be evaluated per row); the CustomScan declines relations with security
  quals, so the ordinary plan applies the policies.
- **SERIALIZABLE.** index_beginscan() takes a relation-level predicate lock on any index whose AM
  has no ampredlocks; the count reads indexes without a scan, so it takes PredicateLockRelation on
  every index it opens, before any lookup, so that absent keys are covered. Without it two
  serializable transactions could each count an absent key, insert it, and both commit.
- **Hot standby.** The pin interlock relies on ambulkdelete taking cleanup locks; WAL replay of
  generic records takes only exclusive locks, so on a standby a reader's pin does not stop replay
  from removing TIDs and the following heap records from setting all-visible. In recovery the count
  therefore treats every heap block as not all-visible and rechecks every candidate TID (still
  correct, no longer O(1) per container). Lifting this needs a custom resource manager whose redo
  takes cleanup locks on container and bucket pages, the way btree_xlog_vacuum does.

Algorithm `lion_count_keys(Relation heap, int nkeys, Relation *indexes, Datum *keys, Snapshot snap)`
1. For each (index, key): locate the entry (bucket head SHARE lock; copy the entry header; for INLINE
   entries copy the payload while holding the pin; for CHAIN entries note head).
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
`lion_index_count(idx1 regclass, key1 anyelement, idx2 regclass, key2 anyelement) RETURNS bigint`.
Both verify the key type matches the index's opcintype, open the heap via IndexGetRelation with
AccessShareLock, use GetActiveSnapshot(), and must return exactly `count(*)` of the equivalent SELECT.
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
    section says about "the relation" then means "the partition being counted".
  - The query has no HAVING, DISTINCT, window functions, grouping sets, ORDER BY inside aggregates,
    FILTER clauses, or aggregates other than `count(*)` and the `count(col)` cases of §14.
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
    indexcollations[0] equals that collation, because the index hashed and compared keys under its
    own collation and the count never rechecks the predicate. Checked per partition.
  - GROUP BY is empty, or exactly one plain Var of the rel with a lion index - or two of them,
    which §20 answers as a nested loop over the two indexes' entries, applying every rule of this
    section per column. A grouped column
    may also appear in the WHERE clause (then it is a single group). §14 removed the `attnotnull`
    requirement: the NULL group comes out of the reserved NULL entry.
  - **The driving index's equality is the grouping equality** (2026-09-20 review, finding 3). An
    index's entries are the classes of
    strategy 1 of ITS opfamily on its own key type, and an operator class is free to define a
    coarser equality than the type's (a text opclass over `lower()` is a valid opclass, and the
    2026-09-20 review built one: 50k `'A'` and 50k `'a'` came out as one group of 100k). So the
    equality operator the planner chose for the grouping column - `SortGroupClause.eqop`, which is
    the type's own - must be exactly the operator
    `get_opfamily_member(idx->opfamily[0], opcintype, opcintype, 1)` returns, compared by Oid;
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
    once the list is longer than the index has buckets, so the pages are `Min(nelems, nbuckets)`;
  - **every page of the group index** for a GROUP BY, at seq_page_cost: its entries are all walked;
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
- ExplainCustomScan: print "Indexes: idx1 (col = const), ..." and "Group Key: col" and, with ANALYZE,
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
answers are checked too.

## 11. Additional VACUUM rule for phase 2 (binding on wave 2 `lion_vacuum.c`)

ambulkdelete acquires a cleanup lock (an exclusive content lock that also waits for all other pins
to drop) on **every page it visits** — every bucket page and every container page of every chain,
whether or not it has anything to remove there — in chain order (bucket pages first, then each chain
left to right). This is the same rule nbtree's btvacuumscan follows, and it is what closes the
page-split hole in the §9 argument: a reader pins page P, copies container C out, and drops the
content lock; a concurrent insert may then split P and move C to a brand-new page N linked
immediately right of P. VACUUM must reach P before N, and P's cleanup lock is blocked by the
reader's pin, so it cannot clean C on N until the reader has finished its visibility-map checks.
If VACUUM had already passed P when the reader copied C, C had already been cleaned of this
cycle's dead TIDs. Two properties make this sufficient: a page is only ever linked into a chain
immediately right of the page whose split or spill allocated it, so chain order and VACUUM's visit
order agree, and each VACUUM cycle's set of dead TIDs is fixed before index cleanup starts. If
VACUUM only cleanup-locked pages it modified, it could walk past an emptied P and clean C on N while
the reader still counts from its stale copy.

Page recycling (§18) does not weaken either property. A recycled block is still linked immediately
right of its split origin, so it is still visited after it. A page only ever LEAVES a chain as part
of a whole-chain free, which happens under a cleanup lock on that page after VACUUM has found it
empty - so a reader pinning it blocks the free outright, and a reader that got past it is holding
containers that VACUUM has already cleaned. And a page that a split moves items onto has had those
items cleaned already, because they come from a page this cycle visited first.

The guarantee readers may rely on is therefore: **a TID is never removed from any page of an index
before VACUUM has held a cleanup lock on every page that precedes it in chain order, and the
removal itself happens under a cleanup lock on that page.** A reader that checks the visibility map
before dropping the pin on the page a container came from cannot be overtaken by VACUUM (§9).

VACUUM waiting rule (binding): VACUUM must never *wait* for a cleanup lock while holding any other
LWLock, because holding an LWLock implies HOLD_INTERRUPTS and the wait becomes uncancellable (a
cursor pinning one container page could then freeze VACUUM until the session ends). The bucket head
lock is needed only while a page is actually modified (the entry counters are written in the same
record), so ambulkdelete processes each bucket in two passes:

  Pass 1 (bucket pages): pin the head; LockBufferForCleanup(head) with nothing else held; process
  every INLINE entry on it (filter, repack, spill if grown); note (offset, head block) of every CHAIN
  entry; release the content lock but keep the head pinned; repeat for further bucket pages.

  Pass 2 (each CHAIN entry's chain, left to right, holding no page lock between steps): for EVERY
  page X of the chain, including pages with nothing to remove:
    1. LockBuffer(head, EXCLUSIVE)  — blocking is fine, nothing else is held.
    2. if ConditionalLockBufferForCleanup(X): filter X; if anything changed, write the fresh entry
       copy (re-read from the page; inserts may have changed it) with the deltas in the same record;
       release X, release head.
    3. else: release head; LockBufferForCleanup(X) (blocking, cancellable, nothing else held);
       if ConditionalLockBuffer(head, EXCLUSIVE): as in step 2; else release X and go to 1.
  A page with nothing to remove still needs the cleanup lock (step 2/3) but no WAL record; the head
  lock may be skipped for such pages only if the page is inspected under the cleanup lock first and
  found clean (then release and move on).
  Inserts to the bucket may interleave between steps; that is safe because splits only move items
  right to new pages (VACUUM revisits or re-filters them) and offsets of entries on bucket pages are
  stable (entries are never deleted). Counters are always applied as deltas to the entry as it is at
  the moment of the write, never as absolutes.

This also removes the latent deadlock between a reader that pins a bucket page (INLINE entry) while
taking a container-page SHARE lock and a VACUUM holding that container page while waiting for the
bucket head: VACUUM never blocks on the head while holding a container page.

Deadlock rule for readers: never acquire a bucket-page lock while holding a pin on a container page.
VACUUM holds the bucket head and then waits for cleanup locks on that bucket's container pages; a
reader holding a container pin and then asking for the bucket head closes the cycle, and buffer
LWLocks have no deadlock detection. Finish with the bucket page (copy the entry out, drop its lock)
before pinning container pages, and never go back. `lion_chain_find_page()` takes SHARE locks
internally, so do not call it while holding a lock on any page of that chain.

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
   keys (§14, amsearchnulls) and the multi-key opclasses of §17. Range predicates (which would need
   entry ordering, and a strategy number outside the 1..5 §17 now uses) are not.
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
is answered whatever its length; the pin budget the cap protects is bounded by the index's bucket
pages instead, since every element's entry lives on one of those.

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
runs at all, `lion_merge_ops()`. Three things were wrong here and all three refused a query the node
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
- Every leaf must be a plain table (relkind `r`; a materialized view is accepted by the same code
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
keys BY THEIR HASH, so the only candidates for equality are adjacent and each key is compared (with
the equality proc, the only ordering-free tool the opclass promises) against the distinct keys of its
own hash run - one of them, except on a hash collision.  A row with n keys therefore costs n hashes,
one sort and about n equality calls; the keys come out in hash order, which nothing depends on.

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

**Reuse.** `lion_new_buffer()`/`lion_new_buffer_xl()` first try GetFreeIndexPage(): take the page with
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
a head.  A reused page always belongs to some other chain, whose head block differs.  *(Deviation
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

## 21. Sorted key directory (format version 4)

Replaces the hash-bucket entry directory of §4 with a B-tree of entries keyed by the index key.

Why. The hash directory has three measured costs: an index created empty keeps its 64 buckets
forever (100k unique keys: 0.104 ms vs 0.321 ms per 100-key IN after growth), a large IN list is one
random bucket page per value (1000 lookups ≈ 1.3 ms), and entry iteration (GROUP BY, IS NOT NULL,
verify) is in hash order, so ordered output and range-bounded entry walks are impossible. A B-tree
directory grows by splitting, makes a sorted IN list a near-sequential leaf walk, and gives GROUP BY
output in key order (a Sort above the node disappears when the query's ORDER BY is the group key).

Structure. Meta page (block 0) points at a root. Directory pages carry LION_PAGE_DIR (internal) or
LION_PAGE_BUCKET (leaf; the flag name is kept so verify/stats/page-inspection tools keep working) in
the special area, plus `level` and left/right sibling links. Leaf pages hold LionEntryTuple items,
exactly as bucket pages do today, sorted by (key, hash) under the opclass ordering; internal pages
hold downlink tuples (separator key + child block), also LionEntryTuple-shaped with a new flag
LION_ENTRY_DOWNLINK so that the key helpers apply unchanged. Reserved NULL and EMPTY entries sort
first (before every value; NULL before EMPTY) via a 1-byte kind prefix in the comparison, not in the
stored key. Multi-key classes: the key type is the STORAGE type (§17), which is what is compared.

Ordering. Every default opclass gains an ordering source: support proc 4 = a btree comparison
function for the key type (the type's default btree opfamily proc 1, resolved at CREATE OPERATOR
CLASS in the SQL script; for citext `citext_cmp`; for anyenum `enum_cmp`). Keys whose type has no
btree opclass fall back to (hash, then bytewise memcmp of the stored datum), which is a total order
that is merely not semantically meaningful; `lion_index_stats()` reports `ordered = false` for such
an index and GROUP BY output is not sorted for it. `amcanorder` stays false (no ordered heap scans:
the AM still emits bitmaps); only the planner's knowledge that a LionCount GROUP BY output is sorted
by the group key is added (pathkeys on the CustomPath for ordered opclasses).

Operations.
- Lookup: descend from the root with binary search per page (SHARE locks, lock coupling parent→child
  as nbtree does, no pins kept above the leaf). The leaf page pin remains the §9 pin for INLINE sets.
- Insert of a new entry: descend with the leaf EXCLUSIVE; on no room, split the leaf (right half to a
  new page, high key to parent, nbtree's "split then insert downlink" ordering with the incomplete
  split bit so a crash between the two records is repaired on the next descent), recursing upward;
  root split creates a new root. Splits move items right only; a page's minimum key never decreases.
  Concurrent readers use right-links: a reader that holds a leaf whose high key is below its search
  key moves right (nbtree's move-right rule), which is what makes a leaf-pin reader safe across a
  concurrent split.
- Entry rewrite in place (payload growth/shrink, spill): as today, on the leaf, EXCLUSIVE; if the
  grown entry no longer fits, split.
- Delete (VACUUM, §18): entries are deleted with PageIndexTupleDeleteNoCompact as today (offsets
  stable for readers parked on the page); an empty leaf is NOT unlinked in this version (leaf
  deletion needs nbtree's half-dead protocol; documented limitation, pages are few).
- Ordered iteration: leftmost leaf, then right-links (SHARE lock one page at a time, pin held while
  an INLINE set from that page is being counted, exactly as the bucket walk does today).
- Bulk build: entries are produced in sorted order by the tuplesort (sort key becomes (kind, key)
  using the ordering proc, hash as tiebreaker), leaves are filled left to right at a fill factor,
  and the internal levels are built bottom-up in the same pass (nbtree's _bt_buildadd shape) through
  the bulk-write API.

IN lists and sums: `lion_posting_set_lookup_many()` sorts the values with the ordering proc and
locates them in one left-to-right leaf walk (re-descend only when the next value is beyond the
current leaf's high key). Disjoint-sum counting (§15) is unchanged.

What goes away: `nbuckets`, `buckets` reloption (accepted and ignored with a NOTICE for one release),
`lion_bucket_of`, bucket sizing in ambuild, the bucket-chain warning; `max_bucket_pages` in stats
becomes `directory_height` and `leaf_pages`.

Locking summary: directory pages: nbtree rules (lock coupling downward, move-right on the leaf,
splits hold left then right then parent). Container chains: unchanged (§5, §11, §18). Lock order
directory page → container pages is preserved; VACUUM's two-pass protocol addresses the leaf page
holding the entry where it used the bucket head.

Format: LION_VERSION 4; version 3 indexes are refused with the REINDEX hint. verify() checks the
tree: keys sorted within and across leaves, high keys consistent with children, every leaf
reachable from the root exactly once, sibling links consistent, level numbers, downlink targets.

## 22. Per-key posting tree (format version 4, same wave)

Replaces the linked chain of container pages per CHAIN entry (§4) with a B-tree over container keys
(ckey), GIN's posting-tree shape.

Why. A chain can only be walked. Every intersection therefore streams the dense sets in full: the
3-column AND at 5M rows visits 570 containers for a 590-row answer, and dirty-heap grouping
re-walks chains per group. With a tree, the AND is driven by the most selective set: for each of
its containers, the other sets are probed by ckey (descend, or step right from the last position).
Cost tracks the selective side. Inserts into the middle of a chain stop being a linear walk from the
head (the churn tests' mid-chain inserts).

Structure. The entry's `head` becomes the posting-tree root (a container page when the tree is one
page: no separate root format, exactly as GIN). Container pages keep their layout (items sorted by
first ckey, minckey/maxckey, owner stamps, DELETED marking) and gain `level` in the special area
(24 → 28 bytes, rounded to 32 by MAXALIGN; adjust LION_SPECIAL_SIZE); internal posting pages hold
(ckey, child block) pairs. Right-links stay, so the sequential walk used by scans, counts and VACUUM
is unchanged: leaves are still a rightlinked list in ckey order.

Operations.
- Descent by ckey (SHARE, lock coupling) to the leaf owning the range; `lion_chain_find_page()`
  becomes a descent instead of a head-to-tail walk; the tail hint stays for appends.
- Leaf split: as today (upper half or insert-position split to a new page linked right), plus a
  downlink insert into the parent with the incomplete-split repair rule; root split allocates a new
  root and the entry's `head` is updated in the same record (entry page, old root, new root, new
  sibling = 4 buffers, the GenericXLog maximum: keep the split's page count at that bound or split
  the record into "split leaf + mark incomplete" and "insert downlink + clear", nbtree-style).
- VACUUM: leaves are visited in ckey order via right-links as today (cleanup lock on every leaf, §11);
  internal pages are cleanup-locked too when traversed; a freed chain frees its internal pages as
  well; internal pages are never deleted while the tree lives (documented limitation).
- Cursors (`LionSetCursor`) gain `seek(ckey)`: descend from the root (or step right while the current
  leaf's maxckey < ckey) and position at the first item with first-ckey ≥ ckey. The AND merge uses it:
  advance the smallest-cardinality cursor sequentially and seek the others. Union and single-set
  counting keep the sequential walk. The §9 pin discipline is unchanged: a leaf stays pinned until
  the container taken from it has passed the visibility-map check; a seek releases the previous
  leaf's pin only after that point (the cursor already has this ordering; seek must use the same
  release point).
- Owner validation (§18) applies to internal pages as well.

Cost model: an AND source's containers term becomes the selective source's container count times
the number of sources (probes), instead of the sum of all sources' containers.

Format: covered by LION_VERSION 4 with §21. verify(): tree shape per key (levels, downlinks, leaf
right-link chain equals the in-order leaf sequence, minckey/maxckey consistent with separators).

Order of work. §21 first (it changes where entries live; the posting tree hangs off the entry and
is independent of the directory shape), §22 second, one format bump. Multicolumn indexes
(`USING lion (a, b, c)`) come after both and mean one directory holding each column's keys as
independent posting sets (order-insensitive, like GIN); a composite-tuple key is deliberately not
offered: a query with one fixed shape is btree's job. The recovery harness must cover
directory splits and posting-tree splits under crash (an injection point between "split page" and
"insert downlink", crash, restart, verify() shows the incomplete-split repair).

## 23. Backlog (not urgent; ordered by when they should happen)

- **HAVING on the count itself** (`GROUP BY k HAVING count(*) > n`): the node knows each group's
  count before emitting it; accept a HAVING that references only the count aggregates and the group
  columns and filter in the node. Today users must write the filter in an outer query.
- **FK-side join pushdown**: `GROUP BY dim.attr` over a fact table joined on a lion-indexed FK column
  is, per dimension group, the union of the member keys' posting sets ANDed with the fact filters.
  Needs the pushdown to accept a subquery-produced key set and the planner to push the aggregate
  through the join.
- **PGXN packaging (before the Citus/TimescaleDB work).** Distribution through the PostgreSQL
  Extension Network is how the two environments below will install it, so it comes first:
  - `META.json` (PGXN Meta Spec v1.0.0): name `pg_lion`, abstract, license `postgresql`, version
    following semver (start at 0.1.0 and bump the control file's `default_version` in step),
    `provides` for both `pg_lion` and `pg_lion_citext` with their control files and SQL scripts,
    `prereqs` (PostgreSQL major range; `citext` for the companion), `resources` (repository,
    bugtracker), tags (index, bitmap, roaring, analytics, count). Validate with `pgxn-utils`
    (`pgxn validate-meta`) or the online validator.
  - Release archive: `make dist` (or `git archive`) producing `pg_lion-<version>.zip` containing
    only what PGXS needs plus README, LICENSE, DESIGN.md and the tests; no `.local`, no benchmark
    results, no `.deps`. `pgxn install pg_lion` must work end to end on a clean machine with the
    packaged `postgresql-server-dev-<N>` headers: no dependency on the source tree, no
    `--enable-injection-points` (test hooks compiled out), no cassert assumptions.
  - Version scripts: once there is a 0.2, `pg_lion--0.1.0--0.2.0.sql` upgrade paths and
    `ALTER EXTENSION UPDATE`; format-version bumps that need REINDEX must say so in the upgrade
    script's NOTICE and in the release notes.
  - Build matrix against packaged PostgreSQL headers for every supported major (not only master):
    a CI job per major running `make installcheck`; decide the minimum supported major (the bulk
    write API needs 17; generic WAL and the CustomScan APIs used are older) and state it in
    `META.json`.
  - `pg_upgrade` across majors with lion indexes present must fail cleanly at the format-version
    check or work, never crash; document REINDEX as the upgrade path for format bumps.
  - Also cover hook coexistence in the same matrix, since PGXN users load pg_lion beside other
    extensions: `create_upper_paths_hook` chaining in both load orders, the reserved GUC prefix,
    and, once the custom rmgr exists, a resource-manager id that does not collide.
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
