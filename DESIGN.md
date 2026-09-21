# roaring_index: a roaring-bitmap inverted index AM for PostgreSQL — design v0

Status: prototype targeting PostgreSQL master (20devel). Built and tested against the assert-enabled
install in `.local/pg` (`.local/pg/bin/pg_config`). This document is the single source of truth for
on-disk format, locking protocol, and module boundaries. Change it before changing the code.

Extension name: `roaring_index`. Access method name: `roaring`. C symbol prefix: `rbi_` / `RBI`.

    CREATE EXTENSION roaring_index;
    CREATE INDEX ON fact USING roaring (country);

## 1. Goals and non-goals for v0

Goals
- Single-column equality index whose posting lists are roaring-style containers (array / bitset / run).
- Bitmap scans (`amgetbitmap`) so the planner can use it in Bitmap Index Scan / BitmapAnd / BitmapOr.
- Correct under concurrent INSERT and VACUUM. Crash-safe via generic WAL.
- Bulk build via tuplesort. Inserts, VACUUM (ambulkdelete) that removes TIDs and shrinks containers.
- SQL-callable `roaring_index_stats()` and `roaring_index_verify()` for tests and debugging.
- Layered so that phase 2 can add: a visibility-map-interlocked `roaring_index_count()` and a
  CustomScan that answers `count(*) ... GROUP BY key` from containers without visiting the heap.

Non-goals for v0 (documented limitations; NULL keys and IN lists arrived in v1, §14 and §15)
- Multi-column indexes, INCLUDE columns, ordered scans, `amgettuple`, parallel build/scan,
  page recycling (freed pages are not returned to the FSM), fine-grained write concurrency
  (inserts serialize per hash bucket), key sizes above 2000 bytes.

## 2. TID encoding

Every heap TID is mapped to a 64-bit code and split into a container key and a 15-bit low part.

    RBI_OFFSET_BITS      = 9 at BLCKSZ 8192 (MaxHeapTuplesPerPage = 291 < 512)
                           10 at 16K (585), 11 at 32K (1169)   -- computed at compile time, see rbi_tid.h
    code(tid)            = ((uint64) block << RBI_OFFSET_BITS) | offset        -- 41 bits at 8K
    RBI_CONTAINER_BITS   = 15
    ckey(code)           = code >> 15                                          -- uint32, block >> 6 at 8K
    lo(code)             = code & 0x7FFF                                       -- 0 .. 32767
    blocks per container = 1 << (15 - RBI_OFFSET_BITS) = 64 at 8K

Offsets ≥ (1 << RBI_OFFSET_BITS) are an ERROR at insert/build time ("table AM not supported").
The meta page records offset_bits and container_bits; opening an index with a mismatch is an ERROR.

Why not GIN's 11 offset bits: bitset containers would be 86% empty. Why 15 container bits and not 16:
a 16-bit bitset is 8192 bytes and cannot be a page item; 15 bits gives a 4096-byte bitset, so every
container fits on any page, and VACUUM can always fall back to a bitset when a run split would
otherwise grow a container past its slot.

## 3. Containers (module `rbi_container.[ch]`, no backend dependencies beyond `c.h` + pg_bitutils)

    typedef struct RBIContainer
    {
        uint32  ckey;          /* container key (code >> 15) */
        uint16  cardinality;   /* number of members, 0..32768 */
        uint8   type;          /* RBI_CT_ARRAY=1, RBI_CT_BITSET=2, RBI_CT_RUN=3 */
        uint8   flags;         /* reserved, 0 */
        /* payload follows immediately (no padding; header is 8 bytes) */
    } RBIContainer;

Payloads (all little values are host-endian uint16/uint64, like every other PG on-disk structure):
- ARRAY:  `uint16 lo[cardinality]`, strictly ascending. Max cardinality RBI_ARRAY_MAX_CARD = 2048.
- BITSET: `uint64 words[512]` (4096 bytes, fixed), bit i set ⇔ lo i is a member.
- RUN:    `uint16 nruns; struct { uint16 start; uint16 len_minus_1; } runs[nruns]`, runs ascending and
          non-adjacent (merged). Max RBI_RUN_MAX_NRUNS = 1023 (payload ≤ 4094 bytes).
- RBI_CONTAINER_MAX_SIZE = 8 + 4096 = 4104 bytes. Invariant: every container ≤ this size.

Representation policy
- Insert into ARRAY beyond 2048 members ⇒ convert to BITSET. Insert into RUN that would exceed
  1023 runs ⇒ convert to BITSET.
- Remove from BITSET leaving ≤ 2048 members ⇒ convert to ARRAY. Remove from RUN that would exceed
  1023 runs (split) ⇒ convert to BITSET. Cardinality may reach 0; the caller deletes empty containers.
- `rbi_container_optimize()` picks the smallest of the three representations. It is called at bulk
  build time for every container and by VACUUM after modifying a container. It is *not* called on
  every insert (inserts only enforce the size invariant), matching CRoaring's runOptimize semantics.
- Mutators operate on a caller-supplied buffer of RBI_CONTAINER_MAX_SIZE bytes. Page code copies a
  container out of the page into such a buffer, mutates, and writes it back (in place when it fits).

Full API: `src/rbi_container.h`. Unit tests: `test/unit/container_test.c` (`make unit`), which must
cover every type transition, boundary cardinalities (0, 1, 2047, 2048, 2049, 32767, 32768 members),
run merging/splitting, and set algebra against a brute-force 32768-bit reference.

## 4. Page layout (module `rbi.h`, implemented in `rbi_pages.c`)

All pages are standard PG pages (PageInit) with a special area:

    typedef struct RBIPageOpaqueData
    {
        BlockNumber rightlink;     /* next page in this chain, or InvalidBlockNumber */
        uint32      minckey;       /* container pages: smallest ckey on page (0 if empty) */
        uint32      maxckey;       /* container pages: largest ckey on page (0 if empty) */
        uint16      flags;         /* RBI_PAGE_META | RBI_PAGE_BUCKET | RBI_PAGE_CONTAINER */
        uint16      page_id;       /* RBI_PAGE_ID = 0xFF87, for identification by inspection tools */
    } RBIPageOpaqueData;

Block 0: meta page. Payload struct `RBIMetaPageData` { magic 0x52424931, version 2, offset_bits,
container_bits, nbuckets, inline_limit, unused padding to 64 bytes }. Version 2 is the first that
indexes NULL keys (§14); a version 1 index is structurally valid but has no NULL entry, so opening
one is an ERROR that asks for a REINDEX rather than a wrong answer to `IS NULL`.

Blocks 1 .. nbuckets: bucket head pages. Bucket b for a key with 32-bit hash h is `h % nbuckets`
(`rbi_bucket_of()` in rbi.h, the single place that mapping lives); its head page is block `1 + b`.
A bucket is a rightlink chain of bucket pages holding entry tuples. nbuckets is NOT a power of two:
ambuild sizes it from the bytes the entries need (§5), and the `buckets` reloption means an exact
count.

Entry tuple (an item on a bucket page):

    typedef struct RBIEntryTuple
    {
        uint32      hash;
        uint16      flags;          /* RBI_ENTRY_INLINE or RBI_ENTRY_CHAIN, plus
                                     * RBI_ENTRY_NULLKEY for the NULL entry (§14) */
        uint16      keylen;         /* bytes of key data stored (0 for the NULL entry) */
        BlockNumber head;           /* CHAIN: first container page; INLINE: InvalidBlockNumber */
        BlockNumber tail;           /* CHAIN: last container page (append hint) */
        uint32      ncontainers;    /* containers in this key's posting set */
        uint64      ntids;          /* members in this key's posting set */
        /* key data: keylen bytes, then MAXALIGN padding */
        /* INLINE only: containers back to back, ascending ckey, total bytes = item size - offset */
    } RBIEntryTuple;                /* header is 32 bytes (RBI_ENTRY_HDRSZ; ntids forces padding) */

Key data is stored with datumCopy semantics: by-value types as a full `Datum` (8 bytes);
fixed-length by-reference types as typlen bytes; varlena as a detoasted, 4-byte-header varlena;
cstring as strlen+1 bytes. Keys larger than RBI_MAX_KEY_SIZE = 2000 bytes are an ERROR.
Entry lookup compares `hash`, then calls the opclass equality operator's function (strategy 1 of the
opfamily, looked up once per relation and cached in `rd_amcache`) with the index collation.

INLINE entries hold their containers in the entry tuple while the payload is ≤ `inline_limit`
bytes (reloption, default 4096 = RBI_MAX_INLINE_LIMIT). When an insert would exceed that, the entry
*spills*: allocate a container page, move the containers there, set head = tail = that page, flags =
CHAIN (keeping RBI_ENTRY_NULLKEY), and shrink the entry tuple. Entries never convert back from CHAIN
to INLINE. The default is the maximum on purpose: a CHAIN posting set owns whole container pages, so
a key whose set is a few hundred bytes costs a whole page once it spills (§13 measured a 20000-key
index at 164 MB with a 1024-byte limit against 35 MB with 4096).

Container pages (per key, CHAIN entries): items are RBIContainer structs, ascending ckey within a
page; all ckeys on page P are smaller than all ckeys on P.rightlink. `minckey`/`maxckey` in the
special area are maintained on every change. Page P owns free space like any heap/index page; use
PageAddItemExtended (with explicit offset to keep order), PageIndexTupleOverwrite (handles size
change), PageIndexTupleDeleteNoCompact/PageIndexMultiDelete + PageRepairFragmentation as needed.

Locating the page for a ckey (`rbi_chain_find_page`): if the tail is non-empty and ckey ≥ tail.minckey
use tail (the append case; an empty tail has minckey 0 and must not be trusted); otherwise walk from
head and stop at the first non-empty page with maxckey ≥ ckey, or the last page.

Growth (`rbi_chain_put_container`): overwrite in place if the page has room; otherwise *split* page P:
move the items at/after the insert position to a freshly allocated page N linked after P; if the
container still does not fit on P, place it alone on a second new page M linked between P and N
(P → M → N → old right). This guarantees progress in one WAL record (P, N, M, entry page = 4 buffers,
the GenericXLog maximum). Items only ever move right and only to brand-new pages. Pages are never
freed or unlinked in v0. `rbi_chain_find_page` skips empty pages (VACUUM may leave them mid-chain).
INLINE payloads are packed without padding, so containers inside them are unaligned: read them with
`rbi_inline_fetch()` into an aligned buffer; never cast into the payload. Containers stored as page
items are MAXALIGNed and may be used in place.

Free space accounting: a container page is "full" for a given container when
PageGetFreeSpace(page) < MAXALIGN(size) + sizeof(ItemIdData). Splits guarantee progress because every
container ≤ 4104 bytes and a fresh page holds at least one.

Page allocation: `rbi_new_buffer()` extends the relation (ExtendBufferedRel), initialises the page
and logs it in its own GenericXLog record; `rbi_new_buffer_xl()` registers the new page in the
caller's record instead, and is what rbi_add_entry and the split path use, so a crash cannot leave an
initialised page that nothing links to. No FSM use in v0; verify() reports unreferenced
never-initialised or empty pages as WARNINGs (harmless, never reused) and anything else as an ERROR.

## 5. Locking protocol (deliberately coarse in v0)

Lock ordering: bucket head page → other bucket pages → container pages (left to right) → new page.
Never lock a page to the left of one you hold. The meta page is read once at relation open and cached.

INSERT (`rbi_insert.c`)
1. Hash the key; lock the bucket head page EXCLUSIVE and hold it until the insert is complete.
2. Walk the bucket chain (lock each further bucket page EXCLUSIVE while inspecting/modifying it;
   pages other than the head may be released when done) to find the entry. If absent, add an INLINE
   entry with one 1-member array container (splitting the bucket chain by appending a new bucket page
   if no bucket page has room).
3. INLINE: rebuild the inline payload in a work buffer with the member added; if ≤ inline_limit and it
   fits on the bucket page (after PageRepairFragmentation if needed), overwrite the entry tuple;
   else spill to a chain (allocate one page, add all containers) and fall through to CHAIN.
4. CHAIN: find the page for ckey; lock it EXCLUSIVE; copy container out, add member (or create a new
   1-member container), write back / split as in §4; update entry (ncontainers, ntids, tail).
   Fast path: a BITSET container has the same size (4104 bytes) whatever its cardinality, so when the
   container already on the page is a bitset the member is set directly in the GenericXLog page image
   (same locks, same single record carrying the entry tuple) instead of being copied out, mutated and
   written back with PageIndexTupleOverwrite. Nothing else on the page changes: same item size, same
   offset, same ckey, so min/max stay put.
5. All page modifications go through GenericXLog: one GenericXLogStart per atomic step, registering
   at most 4 buffers (a split touches P, N, possibly M, and the bucket page).
6. Release everything. Inserts to different buckets do not block each other; inserts to the same
   bucket serialize. Documented and accepted for v0.

SCAN (`rbi_scan.c`, amgetbitmap)
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

VACUUM (`rbi_vacuum.c`, ambulkdelete)
1. For each bucket: two passes as described in §11 (the head is cleanup-locked only while its own
   INLINE entries are modified; chain pages are processed without holding the head across waits).
2. For each entry: INLINE → filter the payload through the callback and repack. Note that removal can
   GROW a container (every-other-member deletion turns a RUN into a 4104-byte BITSET), so a filtered
   INLINE payload may exceed inline_limit or the page: then the entry spills to a chain during VACUUM.
   CHAIN → walk the chain; each container page is locked with LockBufferForCleanup (this is the
   interlock that phase 2 relies on: a heap-skipping reader keeps the page pinned while it consults
   the visibility map). Filter every container with rbi_container_remove_if, run
   rbi_container_optimize, write back in place or, if it grew and no longer fits, re-place it through
   the chain machinery (which may split the page); delete empty containers with one
   PageIndexMultiDelete per page (it compacts; no PageRepairFragmentation needed); update min/max.
   Empty pages stay in the chain. Every page record also carries the updated entry (ncontainers/ntids),
   so a crash cannot desynchronise the counters. Entries whose ntids reaches 0 are kept (the key
   remains known) — v0 does not delete entries.
3. Report stats: num_pages, num_index_tuples = Σ ntids, pages_deleted = 0, tuples_removed.
4. amvacuumcleanup: if stats is NULL (no bulkdelete was needed) return a fresh stats struct by
   counting pages; otherwise pass it through.

BUILD (`rbi_build.c`)
1. table_index_build_scan callback pushes (hash int4, key datum, code int8) into a tuplesort created
   with tuplesort_begin_heap over a 3-attribute TupleDesc, sort keys (hash ASC via int4 btree,
   code ASC via int8 btree), TUPLESORT_RANDOMACCESS, maintenance_work_mem. A NULL key goes in with
   hash 0 and the key column NULL (§14); the two passes below group by the isnull flag first.
2. Pass 1 over the sorted data counts distinct keys (equal hash ⇒ compare with the equality proc,
   remembering the small set of distinct keys seen for the current hash value) and adds up the BYTES
   their entry tuples will need: for each key, MAXALIGN(MAXALIGN(RBI_ENTRY_HDRSZ + keylen) +
   min(payload, inline_limit)) + sizeof(ItemIdData), where the payload of a key with n members is
   estimated as 6n bytes below RBI_SPARSE_THRESHOLD members (one sparse pair each, §13) and
   RBI_CONTAINER_HDRSZ + 2n from there on (an ARRAY container). Both halves are rough - pass 1 does
   not group the codes by container key - but they have the right order of magnitude at both
   extremes. nbuckets = reloption if set, else ceil(total bytes / (BLCKSZ * 3/4)), clamped to
   [RBI_DEFAULT_BUCKETS = 64, 65536]. Bytes rather than key counts, because every bucket owns a head
   page whether it needs one or not: sizing by distinct keys spent 32768 pages (256 MB) on a
   1M-key index whose entries were 84 MB. The floor matters because indexes are usually built on
   empty tables and filled later.
3. tuplesort_rescan; pass 2 groups by key. Because codes are sorted within a hash, and keys sharing a
   hash are rare, keep one open builder per distinct key of the current hash (a builder = ordered
   list of containers under construction; use rbi_container_append_sorted with a per-key "last ckey"
   and finish each container with rbi_container_optimize when the ckey changes). When the hash
   changes, flush all builders: small payload → INLINE entry; else allocate container pages and fill
   them sequentially (fill each page until the next container does not fit; set rightlink,
   min/max), then add the CHAIN entry.
4. Pages are written through the buffer manager; each new page goes through GenericXLog (register,
   fill, finish), or, if !RelationNeedsWAL, without WAL. Meta and bucket pages are created first;
   entry tuples are added to bucket pages under EXCLUSIVE locks like inserts (no concurrency exists
   during build, but the code path is shared).
5. ambuildempty: init meta + bucket pages (nbuckets = reloption or 64) in INIT_FORKNUM with
   log_newpage, as contrib/bloom does.

## 6. Handler settings (`rbi_am.c`)

    amstrategies = 5 (1 equality, 2 @>, 3 &&, 4 <@, 5 @@ -- see §17)
    amsupport = 3 (1 = hash function, same as hash AM; 2 and 3 = GIN's extraction procs, §17)
    amoptsprocnum = 0                amcanorder = false        amcanorderbyop = false
    amcanhash = false                amconsistentequality = true   amconsistentordering = false
    amcanbackward = false            amcanunique = false       amcanmulticol = false
    amoptionalkey = false            amsearcharray = true      amsearchnulls = true
    amstorage = true (§17)           amclusterable = false     ampredlocks = false
    amcanparallel = false            amcanbuildparallel = false    amcaninclude = false
    amusemaintenanceworkmem = true   amsummarizing = false     amkeytype = InvalidOid
    amgettuple = NULL                amgetbitmap = rbigetbitmap    amcanreturn = NULL
    ammarkpos/amrestrpos = NULL      parallel scan callbacks = NULL
    amcostestimate: genericcostestimate() then indexCorrelation = 0 (as contrib/bloom)
    amoptions: reloptions `buckets` (int, 0 = auto, max 65536; any value, not rounded to a power of
    two), `inline_limit` (int bytes, 64..4096, default 4096) and `max_entries` (int, 0 = unlimited,
    §17) via add_reloption_kind / add_int_reloption / build_reloptions.
    amvalidate: a scalar opclass must have support proc 1 with signature (T) → int4 and operator
    strategy 1; a multi-key one (§17) procs 2 and 3 and strategies within {2,3,4,5}.
    Handler follows contrib/bloom in master: `static const IndexAmRoutine amroutine = {...}` returned
    with PG_RETURN_POINTER.

Operator classes (in `roaring_index--0.1.sql`): one DEFAULT opclass per type, reusing the hash AM's
support-1 functions, plus the two multi-key classes of §17. Generate the list from the dev cluster with
`SELECT ... FROM pg_amproc JOIN pg_opclass ... WHERE amname='hash' AND amprocnum=1` for at least:
int2, int4, int8, oid, bool, "char", text, varchar (via text), bpchar, bytea, uuid, date, time,
timestamp, timestamptz, interval, numeric, float4, float8, macaddr, inet, name, jsonb, enum types
via anyenum (hashenum). Strategy 1 operator = the type's `=`.

## 7. SQL functions (`rbi_funcs.c`)

    roaring_index_stats(regclass, OUT nbuckets int, OUT bucket_pages bigint, OUT entries bigint,
        OUT inline_entries bigint, OUT container_pages bigint, OUT containers bigint,
        OUT array_containers bigint, OUT bitset_containers bigint, OUT run_containers bigint,
        OUT ntids bigint, OUT container_bytes bigint, OUT free_bytes bigint,
        OUT sparse_segments bigint, OUT sparse_members bigint, OUT null_tids bigint,
        OUT empty_tids bigint) RETURNS record
        -- container counts include INLINE containers; free_bytes sums bucket and container pages;
        -- null_tids is the member count of the reserved NULL entry (§14) and empty_tids that of
        -- the reserved no-key entry (§17)
    roaring_index_verify(regclass, heapallindexed bool DEFAULT false) RETURNS void
        -- ERRORs on any structural inconsistency: page ids/flags, meta values, entry flags,
        -- ascending ckeys within pages and across rightlinks, min/max correctness, container_check
        -- on every container, ntids/ncontainers sums; with heapallindexed, scans the heap with a
        -- fresh snapshot and checks that every visible tuple's TID is present under its key.
    (phase 2) roaring_index_count(regclass, key anyelement) RETURNS bigint

## 8. Module ownership

    src/rbi_tid.h            TID ↔ code helpers                          (fixed; written by the architect)
    src/rbi_container.h/.c   container library + test/unit/container_test.c   (agent "container")
    src/rbi.h                on-disk structs, RBIState, prototypes of rbi_pages.c (skeleton by architect)
    src/rbi_pages.c          meta/bucket/entry/chain primitives, splits, page alloc   (agent "am-core")
    src/rbi_am.c             handler, options, validate, costestimate, buildempty     (agent "am-core")
    src/rbi_build.c          ambuild                                                   (agent "am-core")
    src/rbi_scan.c           ambeginscan/rescan/endscan/getbitmap                      (agent "am-core")
    src/rbi_insert.c         aminsert                                                  (wave 2)
    src/rbi_vacuum.c         ambulkdelete/amvacuumcleanup                              (wave 2)
    src/rbi_funcs.c          stats/verify (+ count in phase 2)                         (wave 2)
    src/rbi_multikey.c       GIN-style extraction and query trees (§17)                (wave 3)
    roaring_index.control, roaring_index--0.1.sql, Makefile, test/                    (am-core, then wave 2)

Coding conventions: PostgreSQL C style (tabs, K&R braces on their own line for functions, /* */
comments, `elog(ERROR, ...)` for internal errors, `ereport` with errcode for user-facing errors),
compile clean with `-Wall -Wextra -Wno-unused-parameter` and cassert enabled. No CRoaring dependency.

## 9. Phase 2a: heap-skipping count with a visibility-map interlock (`rbi_count.c`)

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

Algorithm `rbi_count_keys(Relation heap, int nkeys, Relation *indexes, Datum *keys, Snapshot snap)`
1. For each (index, key): locate the entry (bucket head SHARE lock; copy the entry header; for INLINE
   entries copy the payload while holding the pin; for CHAIN entries note head).
2. Merge-iterate the k posting sets by ckey (they are sorted). For the AND of k containers with the
   same ckey use rbi_container_and into a work buffer (k-1 times). Containers whose ckey is missing
   from any set contribute nothing.
   Pin discipline: at any time hold at most one pinned page per index (the page whose containers you
   are currently consuming). Advance a set's cursor page only after its containers have been fully
   consumed, and consume means: for the resulting AND container, do the VM checks (step 3) before any
   source page pin is released.
3. For the result container: for each of the ≤ RBI_BLOCKS_PER_CONTAINER heap blocks with members
   (compute member counts per block via rbi_container_range_cardinality over the block's lo range;
   skip blocks with 0), `visibilitymap_get_status(heap, blk, &vmbuf)`; if all-visible: count +=
   members, PredicateLockPage; else: append the block's TIDs (rbi_code_to_tid) to a recheck list.
4. After the merge completes, recheck: sort the TID list (it is already in TID order if produced in
   ckey order), and for each TID call `table_index_fetch_tuple(fetch, &tid, snap, slot, &call_again,
   &all_dead)` in a loop over call_again (HOT chains); count each visible tuple found. Use
   `table_index_fetch_begin/end`. Tuples for which the fetch finds nothing are not counted.
5. Return the count. Memory: recheck list in a per-call context; cap nothing (a worst case of 5% dirty
   pages on 10M rows is 500k TIDs = 3 MB).

SQL surface for tests: `roaring_index_count(idx regclass, key anyelement) RETURNS bigint` and
`roaring_index_count(idx1 regclass, key1 anyelement, idx2 regclass, key2 anyelement) RETURNS bigint`.
Both verify the key type matches the index's opcintype, open the heap via IndexGetRelation with
AccessShareLock, use GetActiveSnapshot(), and must return exactly `count(*)` of the equivalent SELECT.
Isolation tests must cover: concurrent uncommitted insert (not counted), committed insert (counted
in a new snapshot, not in an old REPEATABLE READ one), delete + VACUUM racing with the count (a
cursor or `pg_sleep`-free spec using isolationtester steps: s1 opens a REPEATABLE READ transaction
and counts once, s2 deletes rows and commits, s3 runs VACUUM, s1 counts again and must still see the
old value; then in READ COMMITTED s1 must see the new value).

## 10. Phase 2b: CustomScan for `count(*) [GROUP BY k] FROM t WHERE k1 = c1 AND ...` (`rbi_customscan.c`)

Planner integration
- Install `create_upper_paths_hook` (chaining to any previous hook) at `_PG_init`; act only for
  stage UPPERREL_GROUP_AGG. GUC `roaring_index.enable_count_pushdown` (bool, default on).
- Applicability, all required (bail out silently otherwise):
  - input_rel is a single base relation (RELOPT_BASEREL, RTE_RELATION, relkind ordinary table or
    materialized view) — no joins, no subqueries, no old-style inheritance parents. §16 added
    partitioned parents, which are counted one live leaf partition at a time; everything this
    section says about "the relation" then means "the partition being counted".
  - The query has no HAVING, DISTINCT, window functions, grouping sets, ORDER BY inside aggregates,
    FILTER clauses, or aggregates other than `count(*)` and the `count(col)` cases of §14.
  - Every baserestrictinfo clause is `Var opeq Const` or `Const opeq Var` where Var is a plain column
    of the rel with a *valid* roaring index whose opfamily contains that operator as strategy 1 (use
    the index's opfamily and the operator OID; cross-type integer equality is fine because the
    integer opfamily contains it), the Const is not NULL, and there is at most one clause per column
    (two different constants on one column ⇒ bail; the same constant twice ⇒ dedupe). Parameters
    (Param nodes) may be supported later; v0 = Const only. §15 adds `Var = ANY (Const array)` and
    §14 the two null tests to the shapes accepted here; an `IS NOT NULL` clause is exempt from the
    one-clause-per-column rule, because it constrains no value.
  - GROUP BY is empty, or exactly one plain Var of the rel with a roaring index. The grouped column
    may also appear in the WHERE clause (then it is a single group). §14 removed the `attnotnull`
    requirement: the NULL group comes out of the reserved NULL entry.
  - The planner's grouped_rel target contains only the GROUP BY column(s) and the aggregate(s).
- Add a CustomPath with `flags = 0` (no parallel, no backward, no mark/restore), `pathtarget =
  grouped_rel->reltarget`, rows = estimated groups (from estimate_num_groups, or 1 without GROUP BY),
  startup/total cost = (index pages for the involved keys, estimated as the index size × selectivity,
  clamped ≥ 1) × random_page_cost + containers × cpu_operator_cost + expected recheck TIDs (assume
  the fraction of heap pages not all-visible from `pg_class.relallvisible/relpages`) × cpu_tuple_cost
  + random_page_cost per recheck page. This is deliberately optimistic but proportional; document it.
- PlanCustomPath produces a CustomScan with `scan.scanrelid = 0` (upper-level node),
  `custom_scan_tlist` = the output columns (group key Var(s) with their original varno/varattno, and
  the aggregate as a Const-shaped placeholder replaced at execution), and `custom_private` holding:
  the heap relid, the list of (index oid, attnum, const datum serialized via a Const node) for WHERE,
  the group-by (index oid, attnum) or none, and the aggregate kinds. Use `build_path_tlist`-style
  handling with INDEX_VAR references in the plan's targetlist as pg_strom/TimescaleDB do.

Executor
- BeginCustomScan: open heap with NoLock (the executor already locked every RTE); index_open each
  index with AccessShareLock; register the executor snapshot (estate->es_snapshot); allocate work
  buffers and a per-group memory context.
- ExecCustomScan without GROUP BY: on the first call compute rbi_count_keys for the WHERE keys and
  return one tuple (count); subsequent calls return NULL.
- ExecCustomScan with GROUP BY: iterate all entries of the group-by index in bucket order (walk
  bucket pages; copy each entry's key and head/inline payload while pinned as in §9). For each
  entry, count the AND of its posting set with the WHERE posting sets (§9). Emit (key, count) only
  when count > 0 (a group exists only if at least one row is visible). Output order is arbitrary;
  the planner must not assume sortedness (pathkeys = NIL).
- ReScanCustomScan: reset iteration state. EndCustomScan: close indexes, free.
- ExplainCustomScan: print "Indexes: idx1 (col = const), ..." and "Group Key: col" and, with ANALYZE,
  the number of TIDs rechecked in the heap and heap blocks skipped via the visibility map.

Tests (pg_regress): the pushdown produces identical results to the plain plan for: no rows; all rows;
WHERE constants that match no key; cross-type constants; GROUP BY with and without WHERE; after
DELETE without VACUUM (recheck path) and after VACUUM (VM path); EXPLAIN shows the custom node when
`roaring_index.enable_count_pushdown = on` and the normal plan when off. `test/sql/null.sql` and
`test/sql/inlist.sql` do the same for the clause kinds of §14 and §15, comparing every query against
a forced sequential scan rather than against the pushdown-off plan, so that the access method's own
answers are checked too.

## 11. Additional VACUUM rule for phase 2 (binding on wave 2 `rbi_vacuum.c`)

ambulkdelete acquires a cleanup lock (an exclusive content lock that also waits for all other pins
to drop) on **every page it visits** — every bucket page and every container page of every chain,
whether or not it has anything to remove there — in chain order (bucket pages first, then each chain
left to right). This is the same rule nbtree's btvacuumscan follows, and it is what closes the
page-split hole in the §9 argument: a reader pins page P, copies container C out, and drops the
content lock; a concurrent insert may then split P and move C to a brand-new page N linked
immediately right of P. VACUUM must reach P before N, and P's cleanup lock is blocked by the
reader's pin, so it cannot clean C on N until the reader has finished its visibility-map checks.
If VACUUM had already passed P when the reader copied C, C had already been cleaned of this
cycle's dead TIDs. Two properties make this sufficient: pages are never recycled (a new page is
always linked right of its split origin and visited after it), and each VACUUM cycle's set of dead
TIDs is fixed before index cleanup starts. If VACUUM only cleanup-locked pages it modified, it could
walk past an emptied P and clean C on N while the reader still counts from its stale copy.

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
before pinning container pages, and never go back. `rbi_chain_find_page()` takes SHARE locks
internally, so do not call it while holding a lock on any page of that chain.

## 12. Measured on 20M rows (2026-09-20) and v1 priorities

Measured (optimized build, see README): count pushdown 2.6 ms for a 10M-row key, 50 ms for a 200-group
GROUP BY, 4.9 ms for a 3-index AND; bitmap-scan paths heap-bound at parity with btree/GIN; index build
16-17 s per column; inserts ~4x btree cost. Sizes: clustered keys 10x smaller than GIN, dense random
keys 1.3-2.7x GIN, sparse high-cardinality keys 2-3x GIN.

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
6. Page recycling and entry deletion (v0 never frees pages or entries).
7. Params in the count pushdown; multi-column GROUP BY. Partitions are DONE (§16).
The per-container visibility-map read (rbi_vm_allvisible_mask) is already in: it turned the GROUP BY
from O(heap blocks × keys) (1034 ms) into O(containers) (50 ms).

## 13. Sparse segments (v1 format addition)

Problem (measured, §12): a container costs 12 bytes plus alignment even for one member, so keys with
fewer than ~4 members per 64-page window (high-cardinality columns, tsvector lexemes, c20k/c1m)
cost 24 bytes per row where GIN costs 3.

Item kind. Container pages and INLINE payloads may now hold a fourth item type, the **sparse
segment**, sharing the 8-byte RBIContainer header:

    type        = RBI_CT_SPARSE (4)
    ckey        = first container key covered by the segment
    cardinality = number of members n (1 .. RBI_SPARSE_MAX_PAIRS = 682)
    payload     = uint32 ckeys[n], then uint16 los[n]      -- both sorted by (ckey, lo); 6n bytes
    total size  = 8 + 6n  <= 4104 (same bound as every other item)

A segment covers the ckey range [ckey, ckeys[n-1]]. Invariants: every ckey of a posting set appears
in at most one item (a regular container or inside one segment); items on a page are ordered by first
ckey and their ranges do not overlap or interleave; page minckey/maxckey and chain ordering use each
item's first and last ckey; `rbi_item_size()` dispatches on type for containers and segments.
The entry tuple's `ncontainers` counts ITEMS, containers and segments alike (it is the number the
chain machinery maintains and verify() checks against the items it finds); the `containers`,
`array_containers`, `bitset_containers` and `run_containers` columns of roaring_index_stats() count
only real containers, and `container_bytes` is the bytes of every item, segments included.

Policy. RBI_SPARSE_THRESHOLD = 4: a ckey with ≥ 4 members is a regular container (array cost
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
  half. All through rbi_chain_put_container-style placement so page splits work unchanged.
  Order matters: the threshold test comes FIRST, because promoting a ckey shrinks the segment, so a
  full segment only has to split in half when the pair really stays in it and one insert can never
  produce more than three items. The half-split therefore only happens for a pair that lands inside
  a full segment's range (out-of-order inserts, e.g. heap pages freed by VACUUM and reused); a pair
  beyond the last item simply starts a new segment when the preceding one is full. A full segment
  all of whose pairs share one ckey cannot be split in half; that state is unreachable under the
  policy, and the insert promotes the ckey to a container instead of failing.
  A key's first TID is a one-pair segment (14 bytes), not a one-member ARRAY container (10): one
  more byte per singleton key, six instead of ten for every ckey after it.
  A container built by promoting a ckey out of a segment gets one `rbi_container_optimize()` call,
  exactly as the build path does when it closes a container — that is what makes 200 consecutive
  inserts of one key a 14-byte single-run container instead of a 408-byte array.
- Placement: `rbi_chain_put_items_locked()` replaces the item at an offset (or inserts at it) with
  1..3 items in ONE GenericXLog record, splitting the page exactly as before when they do not fit;
  `rbi_chain_put_container_locked()` is a one-item wrapper over it. Atomicity matters here: a crash
  between writing a promoted container and the remains of its segment would leave a ckey in two
  items, which every reader would then see twice.
- VACUUM: filter pairs; delete empty segments; a regular container that falls below the threshold
  may stay a container (conversion back is optional).
- Scan: emit pairs grouped by heap block via tbm_add_tuples.
- Count (§9): the set cursor presents each ckey of a segment as a temporary ARRAY container built
  on the fly (≤ 3 members), so the merge/AND/VM-mask code is unchanged; one VM mask read per ckey.
- verify: segments sorted, non-overlapping with neighbours, sizes in range, cardinality == n, each
  ckey inside a segment has < threshold members (after build; inserts may transiently violate only
  until extraction). stats: add sparse_segments and sparse_members columns.
- Container library: `rbi_container_check()` rejects type 4 ("item is a sparse segment, not a
  container"); segment helpers live in `src/rbi_sparse.[ch]` with their own standalone unit test
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
RBI_ENTRY_NULLKEY, keylen 0, hash 0, living in bucket 0 and matched by the flag rather than by key.
Everything that searches a bucket for a key skips it (rbi_find_entry_ext), and everything that reads
a stored key asks first; `rbi_find_null_entry()` is the only way to it. Once located it is an
ordinary entry: it spills to a chain, is filtered by VACUUM and is counted exactly like any other,
and the spill keeps the flag.

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
  clauses MINUS that set, which the merge computes per container key with rbi_container_andnot().
  A union of every entry would need one cursor - and up to one buffer pin - per distinct key.
  Any number of `IS NOT NULL` clauses cost one extra cursor each, and no inclusion-exclusion.
- `col IS NOT NULL` with no other clause and no GROUP BY has nothing to drive the merge, so the
  node instead iterates every entry of that column's index and sums the counts (the entries of one
  index are disjoint - a row has one value per column - so the sum is the count of their union),
  with the null entry subtracted as above. EXPLAIN shows this driver as `idx (all keys)`.
- GROUP BY on a nullable column no longer bails: the NULL group is the null entry's count, emitted
  with a NULL datum.
- count(col) is answered when col is declared NOT NULL, or is the group column (count(*) for a real
  group, 0 for the NULL group), or a clause pins it with a strict operator or `IS NOT NULL`
  (count(*)), or a clause says `col IS NULL` (0). The attnotnull requirement of §10 is gone.

## 15. IN lists and ScalarArrayOp (v1, implemented)

AM: `amsearcharray = true`. rbigetbitmap receives an SK_SEARCHARRAY key whose argument is an array
datum: deconstruct it, skip NULL elements, look up each value and emit its set. Duplicates are not
filtered there - set semantics make them harmless, they only cost a second lookup.

Count pushdown: accept `ScalarArrayOpExpr` with useOr = true whose operator is strategy 1 of the
index opfamily and whose array is a non-NULL Const of at most RBI_MAX_ARRAY_ELEMS = 1000 elements;
the clause's posting set is the UNION of the elements' sets. Values are deduped (bytewise, which is
allowed to miss equal-but-not-identical values: a union does not double-count, so a missed duplicate
only costs work). An empty array, an all-NULL one, or a list none of whose values has an entry means
zero rows. Longer lists are left to the ordinary plan, because every listed value needs a posting
set of its own and each may hold a buffer pin for as long as the node runs.

The merge's sources are therefore unions: k sub-cursors merged by container key, yielding
rbi_container_or() of the containers that share the smallest key any of them still has. The §9 pin
rule is per sub-cursor and unchanged - a sub-cursor that contributed to the merged container still
pins the page that container came from, because only rbi_ucursor_next() advances it and the merge
only calls that from rbi_count_container(), after the visibility-map checks.

A GROUP BY over the same column restricts the groups to the listed values (the group's set is
intersected with the union, and a group outside the list counts 0 and is not emitted).
`col = ANY (...)` with useOr = false (`= ALL`) is not pushed down. EXPLAIN prints the list as
`idx (col = ANY ({1,2,3}))`.

## 16. Partitioned tables (v1, implemented)

At UPPERREL_GROUP_AGG the input rel may be a partitioned parent: `rte->inh`, relkind `p`,
`IS_PARTITIONED_REL()` (part_scheme, boundinfo, nparts > 0, part_rels, not dummy), reloptkind
RELOPT_BASEREL. The parent has no storage and, because `get_relation_info()` skips indexes for an
inheritance parent, no `indexlist` either, so everything is resolved per leaf.

Planning (`rbi_try_count_path`, `rbi_collect_targets`)
- The query-level, GROUP BY, WHERE-clause and target-list checks of §10/§14/§15 are unchanged and
  are made against the PARENT: the Vars, attnums and constants the node carries are the parent's.
  Clause analysis no longer looks an index up as it goes; it records (attnum, operator, compared
  type) and the indexes are matched afterwards, once per relation.
- `rbi_collect_targets()` then walks the partition tree and produces one target - heap Oid, the
  index whose entries drive the scan (GROUP BY or the sum-over-all of §14), one index per WHERE
  clause, and the leaf's RelOptInfo for costing - per live leaf partition. It recurses through
  sub-partitioned children, using the planner's already-pruned set (`part_rels[i]` non-NULL and
  `i` in `live_parts`) and additionally skipping children the planner has proved empty
  (`IS_DUMMY_REL`), which count nothing.
- Column numbers are translated for every child through `root->append_rel_array[childrelid]->
  translated_vars`, since partitions may number their columns differently or have dropped ones. A
  column missing in some partition is a bail-out.
- Every leaf must be a plain table (relkind `r`; a materialized view is accepted by the same code
  path but cannot be a partition) with a usable roaring index - the rules of
  `rbi_find_roaring_index` plus, per clause, strategy 1 of THAT index's opfamily for the clause's
  operator and an opfamily member and hash function for the compared type, all checked per
  partition because nothing stops two partitions from using different opclasses. A foreign table,
  or a leaf without one of the indexes, bails out.
- No live partitions (everything pruned) adds no path at all: the planner's own dummy-rel handling
  gives the right answer.
- One CustomPath on the parent's grouped rel. custom_private gains two members: RBI_PRIV_PARTS, a
  list of one OidList per partition (heap Oid, driving index Oid or InvalidOid, then one index Oid
  per WHERE clause in clause order), empty for a plain table; and RBI_PRIV_GROUPKEY, the group Var
  plus the equality operator the planner chose for it. A partitioned plan leaves the index Oids in
  RBI_PRIV_OIDS invalid - there is no single index - and keeps the parent's heap Oid there, which
  is what EXPLAIN resolves column names against.
- Cost: `rbi_cost_count_rel()` is the per-relation estimate of §10, taking one relation's pages,
  allvisfrac and rows and its own indexes; the path's cost is the sum over the leaves. numgroups
  comes from `estimate_num_groups` on the PARENT (a partition may hold rows of every group), and is
  used unchanged for each of them.
- A partitioned GROUP BY needs a hashable grouping column (`SortGroupClause.hashable`), because the
  partitions' groups are merged in a hash table; one table needs no merge and does not care.
- Partitionwise aggregation is left alone: the existing `patype != PARTITIONWISE_AGGREGATE_NONE`
  bail-out means that with `enable_partitionwise_aggregate = on` a partitioned table gets the
  planner's own per-partition Aggs and not this node.

Executor
- `rbi_open_relation()` / `rbi_close_relation()` open and close ONE relation: a plain table once for
  the life of the node, a partition for the length of its own turn. The heap is opened with NoLock -
  the executor holds a lock on every range table entry of the plan, partitions included (the planner
  locked them when it expanded the parent, and `AcquireExecutorLocks()` relocks the whole flat range
  table for a cached plan), and cassert builds check it with `CheckRelationLockedByMe` - and the
  indexes, which are not range table entries, with AccessShareLock.
- Per relation the node then runs one of `rbi_count_relation()` (the intersection of the WHERE
  clauses), `rbi_sumall_relation()` (§14's sum over every entry) or `rbi_group_relation_into_hash()`;
  the single-table paths use the first two unchanged.
- Without GROUP BY the partition counts are summed and one row is emitted, as §10 says.
- With GROUP BY each partition's groups go into a TupleHashTable (`BuildTupleHashTable` over a
  one-column TupleDesc of the group type, with the hash and equality functions
  `execTuplesHashPrepare()` derives from the planner's equality operator, and the group's collation),
  whose per-entry "additional" bytes hold the running int64 count. The NULL group merges like any
  other. Nothing is emitted until every partition has been processed, because a group may have rows
  in any of them; groups whose count is 0 are still not emitted. The table's own contexts (meta,
  tuples, temp) live under the node's `es_query_cxt`; the per-partition work uses the same pergroup
  context as before, reset per group, and the located WHERE payloads are released and their context
  reset at the end of each partition.
- The key a target list prints for a column a clause pins to one value is remembered on the clause
  (`RBIClauseState.storedkey`, copied into a small context of its own) the first time a relation's
  entry has it, because the posting set is gone by the time the row comes out. With partitions that
  is the first partition that holds the key; any other partition's key compares equal to it by the
  index's own equality.
- ReScan throws all of it away - entry scan, posting sets, the open partition, the merged hash table
  and the remembered keys - and starts again.
- EXPLAIN prints `Partitions: p1, p2, ...` in the planner's order. The `Roaring Indexes` line keeps
  its per-clause text but drops the index name for a partitioned scan (`(a), (b = 2)` rather than
  `idx_a (a), idx_b (b = 2)`), because there is one index per partition and no single name to give.
  `Group Key` and the ANALYZE counters are unchanged and are summed over the partitions.

Locking and the §9 pin discipline are per partition and unchanged. A partition's posting sets are
all released before its indexes are closed, so no partition's index pin outlives its turn, and a
VACUUM of one partition cannot affect the count of another: the interlock argument of §9 is about
one heap and its indexes, and each partition is its own.

Not supported: run-time pruning (the node has no Append and no PartitionPruneInfo, so the partition
set is fixed at plan time), Params in the WHERE clauses (§10, not partition-specific), parallel
execution (`flags = 0`, `parallel_safe = false`), and partitionwise aggregation as above. Planning
costs one `index_open` per clause per partition (`rbi_index_bucket_pages` reads the meta page), and
EXPLAIN's `Partitions` line names every one of them, so both are linear in the partition count.

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

The extraction functions are GIN's, named directly in `roaring_index--0.1.sql`:

    CREATE OPERATOR CLASS array_ops DEFAULT FOR TYPE anyarray USING roaring AS
        OPERATOR 2 @> (anyarray, anyarray),
        OPERATOR 3 && (anyarray, anyarray),
        OPERATOR 4 <@ (anyarray, anyarray),
        FUNCTION 2 ginarrayextract(anyarray, internal, internal),
        FUNCTION 3 ginqueryarrayextract(anyarray, internal, int2, internal, internal, internal, internal),
        STORAGE  anyelement;

    CREATE OPERATOR CLASS tsvector_ops DEFAULT FOR TYPE tsvector USING roaring AS
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
  1 &&, 2 @>, 3 <@, 4 =, and tsvector's @@ is its strategy 1.  `rbi_gin_strategy()` in
  rbi_multikey.c is the single place that translation lives.  Getting it wrong is silent - the
  extraction answers for the wrong operator and returns INCLUDE_EMPTY, which only costs a rechecking
  full scan - so it is one function with one switch.

`rbivalidate` has two shapes.  A scalar opclass is checked exactly as before (proc 1 with signature
(T) -> int4, strategy 1 only, every operator's types hashable by the family).  An opclass is
multi-key iff the family has proc 2 for (opcintype, opcintype); then procs 2 and 3 must have GIN's
signatures, every operator's strategy must be in {2,3,4,5}, and proc 1 is required unless
`opckeytype` is polymorphic.  The "every type an operator mentions must be hashable" rule does not
apply to a multi-key family, whose operators mention anyarray and tsquery and whose hashing is of
keys.  `amconsistentequality` stays true: a scalar roaring family holds nothing but equality
operators and a multi-key one holds no equality operator at all, so `equality_ops_are_compatible()`
is never asked about two members of the same roaring family that disagree.

### Key type resolution (`rbi_fill_state`, rbi_pages.c)

The index's own tuple descriptor already carries the resolved key type: `ConstructTupleDescriptor()`
substitutes `opckeytype` for the column type and replaces ANYELEMENT under an ANYARRAY opcintype with
`get_base_element_type()` of the column.  So `RBIState.typid` is still `TupleDescAttr(...)->atttypid`
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

A second key-less entry joins the NULL entry of §14: flag `RBI_ENTRY_EMPTYKEY`, hash 0, keylen 0,
bucket 0, one per index.  It holds the rows a multi-key opclass extracted NO key from - an empty
array, a tsvector with no lexemes - which are under no key at all and which an ALL-mode scan
(`tags @> '{}'`) still has to find.  A NULL column value goes to the NULL entry as before; the two
are mutually exclusive and verify() says so.  `rbi_find_entry_ext()` skips both, and
`rbi_find_reserved_entry()` (with `rbi_find_null_entry()` as a wrapper) is the only way to either.

The meta page version is NOT bumped.  A version 2 index built with a scalar opclass has no empty
entry and needs none - only a multi-key opclass ever writes one, and those did not exist before this
wave - so no existing index answers anything wrongly, which is the test §14 set for a version bump.

`roaring_index_stats()` gains `empty_tids`, the member count of that entry, next to `null_tids`.

### Build and insert

Build (`rbi_build.c`): the tuplesort tuple grows a fourth column, `kind int2`
(REAL / NULL / EMPTY), still sorted by (hash, code).  A multi-key row is pushed once per distinct
extracted key, all with the same code; both passes group by (kind, key) instead of (isnull, key).
Nothing else changes, because the codes of each key still arrive ascending.

Insert (`rbi_insert.c`): `rbiinsert()` extracts and then performs one ordinary single-key insert per
key, each taking and releasing its own bucket lock.  They are not atomic with respect to a reader,
which is exactly the visibility the heap already gives: the inserting transaction has not committed,
so no snapshot that can see the row can run before the last of them is written.

Extraction (`rbi_extract_value()`, rbi_multikey.c) drops NULL keys and duplicates.  A row whose array
holds a NULL element is indexed under its other elements; nothing ever looks for a NULL key, because
a query with one falls back to a rechecking scan.  Duplicates must go, or `rbi_container_add()` would
report "already indexed" and leave `ntids` wrong.  The dedupe is quadratic in the keys of ONE row
(an equality call each), which needs no ordering operator the key type may not have.

`ntids` therefore counts (key, row) pairs, and so does `IndexBuildResult.index_tuples`.  A row with
no keys contributes one pair, in the EMPTY entry.  **VACUUM is unchanged**: its callback is per TID
and a TID is removed from every entry that holds it.

verify(heapallindexed) extracts each heap row's keys with the same function the build uses and checks
that the TID is present under every one of them, or in the EMPTY entry when there are none.

### Queries (`rbi_extract_query()`, rbi_multikey.c)

`searchMode` starts at GIN_SEARCH_MODE_DEFAULT and an out-of-range answer is treated as
GIN_SEARCH_MODE_ALL, as `ginNewScanKey()` does.  The result is one of three modes:

    NONE   nothing matches            DEFAULT mode with zero keys: `tags && '{}'`, an empty tsquery
    KEYS   a boolean tree over the keys, exact
    ALL    every indexed row, with recheck

ALL is the answer for: any searchMode but DEFAULT (INCLUDE_EMPTY and ALL, which is what
`ginqueryarrayextract` returns for `@> '{}'` and for `<@`); any partial-match key (a prefix lexeme -
the keys are hashed, so a range of them cannot be walked); any NULL key; more than
RBI_MAX_QUERY_KEYS = 1000 keys; and a tsquery shape the tree builder rejects.

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

### Bitmap scans (`rbi_scan.c`)

`rbigetbitmap` dispatches on `RBIState.multikey`.  NONE emits nothing; ALL emits the union of every
entry but the NULL one - the EMPTY entry included - with recheck = true (`rbi_emit_all_keys()`, which
is the function `IS NOT NULL` already used); KEYS locates one posting set per key and hands the tree
to the shared evaluator, emitting the result containers with recheck = false.

**The scan reuses the count cursors.**  `rbi_sets_iterate()` (rbi_count.c) walks
(tree over located posting sets) in ascending container-key order and calls back per container; the
scan's callback is `rbi_container_to_tbm()`.  The alternative - one TIDBitmap per key combined with
tbm_intersect/tbm_union - was not needed: the cursors already present a set as one ascending run of
containers, the AND and OR nodes are twenty lines each, and using the same evaluator for the scan and
the count means the two cannot drift apart.

All the posting sets are located BEFORE any of them is walked, so every bucket-page lock the scan
takes is taken before the first container page is pinned: the reader side of the §11 deadlock rule.
`col op ANY (const array)` reaches a multi-key index too (amsearcharray is on for §15's sake); each
element is a query of its own and the answers go into the same bitmap, which is their union.

### The expression evaluator (`rbi_count.c`)

§15's union cursor is generalised into `RBIExprCursor`, a cursor over an `RBIKeyNode` tree whose
leaves are located posting sets.  `RBICountSource` gains a `tree` member; NULL still means "the union
of all the sets", which is what every pre-§17 caller gets.

    LEAF   one RBISetCursor, as before
    OR     stands at the smallest container key any child has left; the container is the OR of the
           children standing there (this is exactly §15's union cursor)
    AND    winds the children forward until they all stand at one container key; the container is
           the AND of theirs, and a container key whose intersection comes out empty is skipped
           here rather than handed up

The §9 pin rule survives both operators: a leaf is only advanced by `rbi_ecursor_next()`, and the
merge only calls that from `rbi_count_container()`, after the visibility map has been consulted - so
every leaf that contributed to a counted container still pins the page that container came from.
The two places an AND lets a pin go without anything having been counted (winding a laggard forward,
dropping an empty intersection) are safe for the same reason the merge's own `!alleq` branch is:
nothing of that container key reaches the visibility map.

Materialization (§9) needed a sharper rule, because with a tree "at least one participating set is
read the pinned way" is no longer implied by counting pinned sets.  `rbi_source_pinned()` decides,
per source, whether EVERY container it can yield comes with a live pin: a leaf has it unless its set
is materialized, an AND has it if ANY child has it (all children stand at the key), an OR only if
EVERY child has it (which children contributed is not known in advance).  A positive source's set is
only materialized while some positive source still has it.

### Count pushdown (`rbi_customscan.c`)

A new clause kind, `RBI_CLAUSE_MULTI`: an OpExpr whose operator is strategy 2, 3 or 5 of some roaring
opfamily, with the column on the LEFT (these operators do not commute: `'{a}' @> tags` is strategy 4)
and a non-NULL Const on the right.  Strategy 4 and everything that is not a roaring operator bail.

- The strategy is read from the OPERATOR (`rbi_op_roaring_strategy()`, a pg_amop lookup restricted to
  the roaring AM), not from an index, because the parent of a partitioned table has no index list
  (§16) and the clause kind has to be known before any index is matched.  `rbi_match_index()` then
  re-checks the strategy against the index that will really answer the clause, per partition.
- The query is extracted AT PLAN TIME and the clause is only pushed down when the mode is KEYS.  An
  ALL-mode query would have every row rechecked in the heap, which is what the ordinary bitmap plan
  already does, better.  `rbi_match_index()` additionally insists that each relation's index carries
  the very extractQuery function the plan-time extraction used, so the run-time extraction cannot
  come out differently.
- At run time the clause's source is the tree over its keys' posting sets, ANDed with the other
  clauses by the merge, exactly like an IN list's union.  Several multi-key clauses on ONE column are
  allowed (the one-positive-clause-per-column rule of §10 is for clauses that pin a value;
  `tags @> '{a}' AND tags && '{b,c}'` is just an AND of three key sets).
- A multi-key clause pins no value, so the column it constrains cannot be printed by the target list;
  it does make the column non-null, so `count(col)` is still answerable.
- **A multi-key index can never DRIVE a count.**  Its entries are keys, not column values, so
  neither §10's GROUP BY (the groups would be lexemes) nor §14's sum-over-all (a row appears under
  each of its keys, so the sum of the entries is not the number of rows) is correct.
  `rbi_find_roaring_index()` takes a `multikey` flag and the driving lookup passes false, which makes
  `count(*) WHERE tags IS NOT NULL` fall back to the ordinary plan.  GROUP BY on a scalar roaring
  column next to a multi-key WHERE clause is the supported and tested combination.
- EXPLAIN prints the clause with its operator: `Roaring Indexes: idx (tags @> {t5,t7})`,
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
count pushdown (only in the bitmap scan); a multi-key index as the GROUP BY or sum-over-all driver;
`roaring_index_count(idx, key)` on a multi-key index (it needs a strategy-1 operator and errors out).

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
