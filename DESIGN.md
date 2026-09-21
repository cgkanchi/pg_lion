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

    amstrategies = 1 (equality)      amsupport = 1 (support proc 1 = hash function, same as hash AM)
    amoptsprocnum = 0                amcanorder = false        amcanorderbyop = false
    amcanhash = false                amconsistentequality = true   amconsistentordering = false
    amcanbackward = false            amcanunique = false       amcanmulticol = false
    amoptionalkey = false            amsearcharray = true      amsearchnulls = true
    amstorage = false                amclusterable = false     ampredlocks = false
    amcanparallel = false            amcanbuildparallel = false    amcaninclude = false
    amusemaintenanceworkmem = true   amsummarizing = false     amkeytype = InvalidOid
    amgettuple = NULL                amgetbitmap = rbigetbitmap    amcanreturn = NULL
    ammarkpos/amrestrpos = NULL      parallel scan callbacks = NULL
    amcostestimate: genericcostestimate() then indexCorrelation = 0 (as contrib/bloom)
    amoptions: reloptions `buckets` (int, 0 = auto, max 65536; any value, not rounded to a power of
    two) and `inline_limit` (int bytes, 64..4096, default 4096) via add_reloption_kind /
    add_int_reloption / build_reloptions.
    amvalidate: opclass must have support proc 1 with signature (T) → int4 and operator strategy 1.
    Handler follows contrib/bloom in master: `static const IndexAmRoutine amroutine = {...}` returned
    with PG_RETURN_POINTER.

Operator classes (in `roaring_index--0.1.sql`): one DEFAULT opclass per type, reusing the hash AM's
support-1 functions. Generate the list from the dev cluster with
`SELECT ... FROM pg_amproc JOIN pg_opclass ... WHERE amname='hash' AND amprocnum=1` for at least:
int2, int4, int8, oid, bool, "char", text, varchar (via text), bpchar, bytea, uuid, date, time,
timestamp, timestamptz, interval, numeric, float4, float8, macaddr, inet, name, jsonb, enum types
via anyenum (hashenum). Strategy 1 operator = the type's `=`.

## 7. SQL functions (`rbi_funcs.c`)

    roaring_index_stats(regclass, OUT nbuckets int, OUT bucket_pages bigint, OUT entries bigint,
        OUT inline_entries bigint, OUT container_pages bigint, OUT containers bigint,
        OUT array_containers bigint, OUT bitset_containers bigint, OUT run_containers bigint,
        OUT ntids bigint, OUT container_bytes bigint, OUT free_bytes bigint,
        OUT sparse_segments bigint, OUT sparse_members bigint, OUT null_tids bigint) RETURNS record
        -- container counts include INLINE containers; free_bytes sums bucket and container pages;
        -- null_tids is the member count of the reserved NULL entry (§14)
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
    materialized view) — no joins, no subqueries, no inheritance/partition parents.
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
   keys (§14, amsearchnulls). Range predicates (strategy 2..5 via entry ordering) are not.
6. Page recycling and entry deletion (v0 never frees pages or entries).
7. Params and partitions in the count pushdown; multi-column GROUP BY.
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

## 16. Partitioned tables (v1)

At UPPERREL_GROUP_AGG the input rel may be a partitioned parent (`rte->inh`, part_scheme set). Every
live leaf partition (recursively through sub-partitioning, using the planner's pruned
`part_rels`/live_parts) must have a usable roaring index on the same column, with attnos mapped
through each partition's AppendRelInfo translated_vars (partitions may have different attnums), and
all WHERE columns likewise. One CustomPath on the parent's grouped rel; custom_private lists
(child relid, heap oid, group index oid, attno, WHERE index oids). Executor: for each partition,
open its heap and indexes (the executor already locked every child RTE), count or group with the
same code as a single table; GROUP BY merges counts across partitions in a TupleHashTable built with
the group column's equality/hash operators (execGrouping.c), emitting when all partitions are done.
No run-time pruning (Params are not supported anyway). EXPLAIN lists the partitions.

## 17. Multi-key operator classes: arrays and tsvector (v1, after §13-§16)

GIN-style extraction so one row can contribute many keys. Opclass support procs: 1 = hash of the
key type; 2 = extractValue(datum) → keys[] (+ nulls); 3 = extractQuery(query, strategy) → keys[] and
a mode: AND (every key must match; used by `anyarray @> anyarray`), OR (`anyarray && anyarray`),
ALL_WITH_RECHECK (scan every indexed row and recheck the operator; used for `@> '{}'`, `<@`, and
tsquery with NOT, phrase or prefix operators), or a boolean tree for tsquery AND/OR of plain
lexemes. `amstorage = true`; the entry key type is the element type (text for tsvector lexemes).
Strategies: 1 =, 2 @>, 3 &&, 4 <@, 5 @@ (tsvector). Build/insert produce (key, code) pairs per
extracted key; a row with no keys produces nothing (so `@> '{}'` needs ALL_WITH_RECHECK). VACUUM is
unchanged. Bitmap scans combine sets per the mode (AND via rbi_container_and per ckey, OR via
rbi_container_or), recheck only in ALL_WITH_RECHECK mode. Count pushdown: `count(*) WHERE tags @>
'{a,b}'` is the AND of element sets (exact), `&&` the union; tsquery trees of AND/OR over plain
lexemes map onto the same cursors; anything needing recheck is not pushed down. Cardinality guard:
reloption `max_entries` (default 0 = unlimited) emits one WARNING per backend when exceeded during
build or insert; it never rejects rows. Sparse segments (§13) are what keep hundreds of thousands
of lexeme entries at GIN-like size.
