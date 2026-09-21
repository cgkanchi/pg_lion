> Note: the historical sections below predate the rename to **pg_lion** (2026-09-21). Names map as: extension `roaring_index` -> `pg_lion`, access method `roaring` -> `lion`, functions `roaring_index_*` -> `lion_index_*`, GUC `roaring_index.*` -> `pg_lion.*`, C prefix `rbi_`/`RBI` -> `lion_`/`LION`/`Lion`, files `src/rbi_*.c` -> `src/lion_*.c`.

Codebase review — latest verification 2026-09-21

**Current status at `481f876`: one P2 finding remains.** Original findings **1–7 are addressed in the reviewed paths**, including bounded partition aggregation and grouped-count costing. The follow-up's multikey fallback costing is also corrected. Original finding **8 remains partially addressed**: startup and endpoint-identity failures are safe, but failure to capture the original invalid-index state is swallowed, allowing cleanup to promote previously invalid indexes. This was reproduced with fake executables, without touching a database.

Fresh validation passes **17 SQL regressions, 6 isolation tests, both unit suites, and the crash-recovery/hot-standby harness**. The stale-statistics partition reproduction now spills through core Finalize HashAggregate and exactly matches ordinary aggregation. See [FOLLOWUP_REVIEW.md](FOLLOWUP_REVIEW.md) for the current finding, focused performance measurements, quick-benchmark results, and evidence. Historical reports below are retained; no extension implementation fixes are included in this verification.

The rest of this document is the **historical review of `609babec`**, preserved as the record that motivated the fixes. Its findings, line numbers, and conclusions describe that earlier commit, not the current implementation.

---

Original codebase review — 2026-09-20

Reviewed commit: `609babec0c097628ee339643d6d304cdc13432ba`.

The bitmap engine has a useful separation from PostgreSQL storage, substantial tests, and a demonstrated fast count path. I would keep the extension at prototype status: this review reproduced index corruption and three classes of incorrect query results. The highest priorities are preserving reserved-entry flags, enforcing snapshot eligibility, and making grouping obey SQL equality and value-representation semantics.

Eight findings follow. P1 means fix before relying on the affected feature; P2 means a material reliability or performance issue. Five findings were reproduced with SQL, one was measured with EXPLAIN, and two are established from source inspection. No implementation fixes were made.

1. **[P1] Spilling an empty-key posting set corrupts its entry identity.**

   Location: [rbi_pages.c:1298](/home/cgkanchi/code/pg_roaring_index/src/rbi_pages.c:1298).

   `rbi_entry_spill()` preserves only `RBI_ENTRY_NULLKEY` when changing INLINE to CHAIN. It discards `RBI_ENTRY_EMPTYKEY`, although both are reserved entries with no key bytes. Subsequent empty-array/empty-tsvector inserts cannot find the old posting set and create another entry. The old entry is interpreted as a normal key despite having `keylen == 0`.

   Reproduced with default options: create an empty `int[]` table and roaring index, then insert 10,000 rows alternating `'{}'` and `'{1}'`. Statistics report four entries and only 910 empty TIDs instead of two entries and 5,000 empty TIDs. With `inline_limit=64`, just 1,000 alternating rows produced 19 entries, seven recognized empty TIDs, and `roaring_index_verify()` failed with `key length 0, expected 1 .. 2000`. The fallback full scan still counted all rows in this test; that does not make the stored entries valid.

   Preserve `RBI_ENTRY_RESERVED` through the transition. Add INSERT-triggered and VACUUM-triggered spill coverage for both reserved kinds. Existing damaged indexes need rebuilding after the fix. [Reproduction](/tmp/rbi-review/empty2.sql), [output](/tmp/rbi-review/empty2.log), [default-options output](/tmp/rbi-review/empty_default.log).

2. **[P1] SQL count functions bypass the index's snapshot horizon.**

   Locations: [rbi_count.c:2505](/home/cgkanchi/code/pg_roaring_index/src/rbi_count.c:2505), [existing verifier check](/home/cgkanchi/code/pg_roaring_index/src/rbi_funcs.c:1065).

   The direct SQL count path opens the index and uses the active snapshot without checking `indcheckxmin`. A newly built index can omit an old HOT-chain version that a repeatable-read transaction still sees. Heap rechecking cannot recover a TID absent from the selected posting set.

   Reproduced with two sessions: create a row `k=1`; session B establishes a repeatable-read snapshot; session A HOT-updates it to `k=2` and creates the roaring index. The index has `indisvalid=t`, `indisready=t`, and `indcheckxmin=t`. In B, ordinary `count(*) WHERE k=1` returns **1**, while `roaring_index_count(index,1)` returns **0**. The verifier correctly refuses the same snapshot. This affects the shared implementation of all three SQL count functions.

   Centralize direct-index eligibility checks and reject an unsafe snapshot before lookup. Also check index validity/readiness rather than assuming the direct SQL caller received a planner-approved index. The latter is an additional missing check; the reproduced wrong result specifically concerns `indcheckxmin`. [Two-session reproduction](/tmp/rbi-review/snapshot.py), [output](/tmp/rbi-review/snapshot.log).

3. **[P1] GROUP BY accepts an index with incompatible equality semantics.**

   Location: [rbi_customscan.c:705](/home/cgkanchi/code/pg_roaring_index/src/rbi_customscan.c:705).

   The driving index is checked for column, scalar shape, and collation, but its equality operator is not checked against the grouping equality operator. Matching collation is insufficient: an opclass may define a different equivalence relation on the same type.

   Reproduced with a valid text opclass using `lower(a)=lower(b)` and `hashtext(lower(a))`. For 50,000 `'A'` rows and 50,000 `'a'` rows, ordinary text grouping returns two groups of 50,000; RoaringCount returns one group of 100,000. The custom path was selected without forcing it. Cross-partition merging cannot reconstruct groups already combined inside an incompatible posting set.

   Pass the GROUP BY equality operator into index selection and prove compatibility before accepting the index. Otherwise retain the ordinary aggregate. [Reproduction](/tmp/rbi-review/semantics.sql), [output](/tmp/rbi-review/semantics.log).

4. **[P1] GROUP BY can emit a value that no visible row contains.**

   Locations: [stored-key copy](/home/cgkanchi/code/pg_roaring_index/src/rbi_count.c:309), [group output](/home/cgkanchi/code/pg_roaring_index/src/rbi_customscan.c:2708).

   Posting sets store one historical representative per equality class. The count path checks whether the group contains visible rows, but emits the stored representative without proving that its representation occurs in those rows. This is distinct from incompatible grouping equality: it happens with the shipped `citext` opclass and matching grouping semantics.

   Reproduced by indexing `'SecretOldSpelling'`, deleting it, inserting 10,000 copies of `'secretoldspelling'`, and vacuuming. RoaringCount returns **`SecretOldSpelling`**; the ordinary aggregate returns **`secretoldspelling`**. An outer `left(group_key::text,1)` produces **`S` versus `s`**, so the difference survives into ordinary case-sensitive SQL operations.

   Only emit an index representative when the type/opclass guarantees that equality preserves the relevant value representation. Otherwise obtain a visible representative from a matching heap row, or decline value-producing pushdown. Apply the same rule to columns emitted from equality-clause entries. [Reproduction](/tmp/rbi-review/representative.sql), [output](/tmp/rbi-review/representative.log).

5. **[P2] The dirty-page cost estimate can suppress the main optimization on a freshly vacuumed table.**

   Location: [rbi_customscan.c:894](/home/cgkanchi/code/pg_roaring_index/src/rbi_customscan.c:894).

   `recheck_pages = Min(recheck_tids, heap_pages)` can charge random reads across the entire heap even when the same statistics say only a small fraction of its pages are not all-visible. For a single count, the per-block recheck implementation visits each dirty block at most once; its estimated page count should respect that bound.

   On a freshly vacuumed one-million-row table, statistics reported 4,480 pages and 4,425 all-visible pages. The model priced RoaringCount at **18,054.56**, above a bitmap aggregate at **16,247.09**. The forced RoaringCount actually visited 70 containers, used 71 shared-buffer hits, rechecked **zero** heap TIDs, and finished in **0.244 ms**. Ordinary serial bitmap aggregates took **50–62 ms**; the default parallel sequential plan took roughly **30 ms**. These are local diagnostic timings, not production throughput estimates.

   Bound the single-count page estimate by the estimated non-all-visible pages. Model repeated per-group visits separately, and include the startup/memory cost of the fully materialized partition-group merge. Validate plan choice across high visibility fractions as well as completely clean and dirty tables. [Plans and timing](/tmp/rbi-review/cost.log), [ordinary plans](/tmp/rbi-review/planner.log).

6. **[P2] Count execution has no overall memory budget.**

   Locations: [TID-array growth](/home/cgkanchi/code/pg_roaring_index/src/rbi_count.c:1540), [deferred heap recheck](/home/cgkanchi/code/pg_roaring_index/src/rbi_count.c:2116), [partition group hash](/home/cgkanchi/code/pg_roaring_index/src/rbi_customscan.c:1831).

   Every non-all-visible candidate TID is retained until the entire posting-set merge finishes. The array doubles without consulting `work_mem` or processing completed blocks incrementally. On the reviewed build, 20 million candidates require a 33,554,432-element allocation: **192 MiB for the TID array alone**. Growth beyond 134,217,728 candidates requests a 1.5-GiB allocation and exceeds PostgreSQL's normal allocation limit. Hot standby deliberately sends every candidate through this path.

   Partitioned GROUP BY also retains all distinct groups in a TupleHashTable, with no executor spill path or budget enforcement. Small per-posting-set materialization caps do not bound either of these structures.

   Recheck completed blocks in bounded batches, preserving the established pin/visibility-map ordering. Budget the cross-partition aggregate and spill or reject the custom path when appropriate. This finding is based on allocation and control-flow inspection; an OOM-scale test was deliberately unnecessary.

7. **[P2] The cached access-method OID survives DROP/CREATE EXTENSION.**

   Location: [rbi_count.c:209](/home/cgkanchi/code/pg_roaring_index/src/rbi_count.c:209).

   `rbi_get_am_oid()` caches a process-local OID forever. Relation-cache invalidation does not reset this static variable. Dropping the extension with CASCADE removes its indexes, after which recreating it legitimately allocates a new access-method OID.

   Reproduced in one backend: count successfully, drop/recreate the extension, recreate the index, and count again. The function now errors **`index "t_idx" is not a roaring index`**. Planner matching also compares against the stale OID.

   Use a catalog lookup backed by PostgreSQL's existing cache, or register invalidation for this cached identity. [Reproduction](/tmp/rbi-review/recreate.sql), [output](/tmp/rbi-review/recreate.log).

8. **[P2] The benchmark launcher can drop indexes in a different cluster than the supplied data directory.**

   Location: [bench/run_bench.sh:14](/home/cgkanchi/code/pg_roaring_index/bench/run_bench.sh:14). The container-bit experiment repeats the pattern.

   The script ignores `pg_ctl start` failure, connects to a hardcoded socket/port, and immediately executes `DROP EXTENSION roaring_index CASCADE`. If another cluster already owns that endpoint, startup of the requested cluster fails and the destructive command reaches the existing cluster instead. Suppressing startup failure makes this a concrete target-selection error.

   Allocate an isolated endpoint, fail on unexpected startup failure, and verify `SHOW data_directory` against the requested directory before destructive SQL. Use `ON_ERROR_STOP` and cleanup traps. The benchmark's direct `pg_index.indisvalid` changes should also be restored on failure. This finding was established by reading the scripts; the destructive scenario was not executed.

**Architecture assessment.** The frontend-compatible container/sparse libraries are the strongest boundary: representations and set operations are independently testable. The page layer centralizes key storage, posting transitions, splits, and generic WAL. The explicit pin discipline, two-pass cancellable VACUUM protocol, and isolation injection points are valuable foundations. Keep those invariants explicit when changing storage.

The weakest boundary is between index membership, SQL semantics, and the custom aggregate. An equality class is sufficient to count members but insufficient to reconstruct every original value. Likewise, a structurally valid index is not automatically usable by every snapshot. Share eligibility checks among the planner, SQL functions, and verifier, and give value-producing pushdown a stricter contract than count-only pushdown.

The two largest integration modules, `rbi_count.c` and `rbi_customscan.c`, contain about 5,500 lines together. Useful extraction boundaries are planner eligibility/costing, posting expression cursors, visibility/recheck batching, and aggregate execution. The existing positional `custom_private` lists make planner/executor changes harder to audit; centralized encoding/decoding and an explicit version/shape check would reduce that risk. Refactoring should follow the correctness fixes so it does not obscure them.

Several existing design choices impose substantial workload limits:

- **Fixed hash buckets.** An index built on an empty table starts with 64 buckets and never grows its directory. Lookups scan bucket entries linearly while inserts hold the bucket head exclusively. With 200,000 distinct keys, two warmed runs of 10,000 point counts took **88.2/88.4 ms** with 64 buckets versus **33.0/30.9 ms** with 1,693 automatically sized buckets on the same data. This isolates the directory-size effect; it is not an insert-throughput measurement. Track chain lengths and provide a rebuild/growth policy. [Benchmark SQL](/tmp/rbi-review/performance.sql), [results](/tmp/rbi-review/performance.log).
- **Historical growth.** VACUUM retains entries and does not recycle pages. Changing-key workloads accumulate historical dictionary entries and empty chain pages even when live cardinality is stable. Existing chain lookup walks from the head for non-tail keys. Plan a sustained churn benchmark and report dead-entry/empty-page metrics; index size after a static build is not enough.
- **Storage occupancy.** A 4,104-byte bitset cannot share an 8-KiB page with another such bitset, and a spilled key owns its container pages. The documented size penalties follow directly. Shared storage could improve density, but page recycling, sharing, or moving items left changes the VACUUM/pin proof and cannot be treated as a local allocator optimization.
- **Multi-key extraction.** [rbi_extract_value()](/home/cgkanchi/code/pg_roaring_index/src/rbi_multikey.c:210) checks each extracted key against every earlier distinct key, giving quadratic hash-comparison work before insertion. A temporary hash set or sort/deduplicate step would bound large-array/document extraction better. This hotspot was identified statically, not separately benchmarked.
- **Write contention.** Bucket-exclusive insertion particularly limits concurrency for the low-cardinality columns where count acceleration is attractive. Batched ingestion or pending postings are possible directions, but require their own snapshot and VACUUM design. The current implementation is best aligned with bulk-loaded, read-heavy analytical data.

**Validation performed.** Tests used a separate temporary PostgreSQL cluster at `/tmp/rbi-review/data`, port 55439, with the workspace PostgreSQL 20devel installation. The server was built with assertions, debugging, injection points, and `-O1`; ICU was unavailable. Durability settings used the temporary cluster's defaults. Existing development data was not used for the tests.

- A clean extension build from copied source completed without compiler warnings. [Build log](/tmp/rbi-review/clean-build.log).
- All **17 SQL regression tests** and **5 isolation tests** passed. [Test log](/tmp/rbi-review/installcheck.log).
- Both standalone unit suites passed: **950,294 checks** and **51,197,308 reference member/pair comparisons** in total.
- Both standalone suites also passed AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer was disabled because the sandbox's tracing environment prevents it from running. The PostgreSQL server itself was not sanitizer-instrumented. [Container log](/tmp/rbi-review/container_san.log), [sparse log](/tmp/rbi-review/sparse_san.log).
- Targeted SQL reproduced findings 1–4 and 7. Targeted timings support finding 5 and the fixed-bucket observation. Dropping a normally referenced index after preparing a count query correctly invalidated/replanned it in the tested case.

The passing suites do not cover the reproduced boundary cases. Add those regressions first, then low-memory dirty-count/partition-group tests, high-visibility plan-choice tests, and sustained insert/update/delete/VACUUM benchmarks. Before treating this as production-ready, automate crash/recovery and standby tests under concurrent writes, and measure WAL volume, cold-cache latency, concurrent throughput, and memory on a release build. This review did not rerun the historical 20-million-row suite, perform a fresh crash/standby campaign, or validate other PostgreSQL versions/page sizes.

The practical order is: repair findings 1–4 and rebuild affected indexes; fix eligibility/cache and resource budgets; correct the cost model and benchmark isolation; then evaluate storage growth and concurrency changes against the documented interlock invariants. The temporary reproduction scripts and logs linked above remain under `/tmp/rbi-review`; they are review artifacts, not additions to the automated suite.
