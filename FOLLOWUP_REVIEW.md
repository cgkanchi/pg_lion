# Review verification — 2026-09-21, `481f876`

**Not all findings are fixed: one P2 remains in the legacy benchmark launcher.** Original findings 1–7 and the additional multikey-cost finding are addressed in the reviewed paths. Original finding 8 is partially addressed: startup and identity failures now prevent cleanup writes, but a failed original-state capture still permits unsafe restoration. This verification changes review documents and reproductions, not the extension implementation.

## Remaining finding

### [P2] Failed capture of original index validity permits unsafe cleanup

Locations: [original-state capture](bench/lib.sh#L69), [restoration](bench/lib.sh#L24).

`bench_start_cluster()` sets `BENCH_VERIFIED=1` before reading the initially invalid indexes. The capture command ends in `|| true`, so a failed query becomes an empty `BENCH_ORIG_INVALID`, indistinguishable from a successful query that found none. Execution continues. On exit, `bench_restore_indexes()` can reconnect and issue an unrestricted `UPDATE pg_index SET indisvalid=true` for every invalid `fact_%` index. A previously invalid or incomplete index can become eligible for query planning even though this run never invalidated it.

The updated [stub reproducer](test/review/benchmark_cleanup.py) uses fake `pg_ctl` and `psql`, with no database connection. With startup and identity checks succeeding but the state-capture query failing, the benchmark body is reached, cleanup emits the broad catalog UPDATE, and the launcher exits successfully. The reproducer deliberately exits **1** to flag this unresolved finding. [Captured evidence](test/review/evidence/481f876/cleanup-repro.txt) also confirms that failed startup and wrong identity issue **no cleanup writes**, and successful state capture excludes the recorded preexisting invalid index.

Fail closed when state capture fails, and gate restoration on successful capture as well as endpoint verification. Prefer tracking exactly the index OIDs/state transitions owned by the run, or separate portfolios without catalog edits. The legacy body's `valid()` helper in [bench/bench.sh](bench/bench.sh#L16) also still enables every matching family index; the captured initial invalid set only protects the cleanup query, not that helper. The comprehensive and quick runners use separate portfolios in fresh private clusters and do not use this catalog-toggling path.

## Finding status and evidence

| Finding | Status at `481f876` | Current evidence |
| --- | --- | --- |
| Original 1: reserved entry identity on spill | Addressed | Spill preserves `LION_ENTRY_RESERVED`; array, tsvector, NULL, insertion and verifier regressions pass. |
| Original 2: direct count snapshot eligibility | Addressed | Shared `lion_index_usable()` checks validity/readiness and `indcheckxmin`; `count_checkxmin` and snapshot isolation tests pass. |
| Original 3: incompatible grouping equality | Addressed | Grouping index selection checks compatible equality; incompatible-opclass regression passes. |
| Original 4: nonvisible historical group representative | Addressed | Representation-safety guard remains; citext and pushdown regressions pass. |
| Original 5 / follow-up 3: dirty grouped-count costing | Addressed for reproduced case | Cache-aware distinct-page costing and bounded visibility cache; default chooses LionCount on the original 5M/200-group fixture. |
| Original 6 / follow-up 1: unbounded grouping/rechecks | Addressed | Bounded dirty-TID batches remain; partition node streams partial counts into core Finalize HashAggregate, which spills even with stale statistics. |
| Original 7: stale cached AM OID | Addressed | Catalog lookup remains; drop/recreate regression passes. |
| Original 8 / follow-up 2: benchmark endpoint/state safety | Partially addressed | Startup and identity failure cases pass; failed original-state capture remains unsafe as described above. |
| Follow-up 4: multikey fallback costing | Addressed for reproduced cases | Full-walk index cost and full candidate coverage are charged; default phrase/prefix plans now use sequential scans. |

The [stale-statistics partition reproduction](test/review/evidence/481f876/partition-estimate.txt), with an estimate of ten groups and 20,010 actual groups, now shows **Finalize HashAggregate over LionCount, 534 batches and 968 kB spilled**, with the same 112 kB reported memory usage as ordinary HashAggregate. This fixes the extension-owned unbounded hash; it does not claim PostgreSQL's reported aggregate memory is a strict 64 kB cap. The old group-hash contexts are absent, and all 20,010 result groups match ordinary aggregation exactly. The regression suite also covers partition spill/rescan behavior.

## Focused performance verification

Clean builds were installed separately into assertion-enabled and release PostgreSQL 20devel prefixes. Focused timings use the release build, ten randomized interleaved rounds per mode, identical fixtures to the earlier reproductions, and exact-result comparisons. [Build and binary provenance](test/review/evidence/481f876/provenance.json).

| Case | Default route | Default median | Control median |
| --- | --- | ---: | ---: |
| 5M rows, 200 groups, first 5% updated | LionCount | 55.340 ms | 917.707 ms sequential; 53.341 ms with sequential scans discouraged |
| 200k documents, `common <-> w1` | Sequential scan | 33.199 ms | 33.420 ms forced sequential |
| 200k documents, `rare12:*` | Sequential scan | 24.426 ms | 24.748 ms forced sequential |

The grouped-count case retains 91,345 all-visible pages out of 100,407. A default sample has 395,636 visibility-cache hits and zero shared-buffer reads; the sequential control reads more of the heap than fits in shared buffers. These are warmed runs without deliberate OS-cache eviction, not an all-pages-resident comparison. The planner now estimates LionCount at about 17,286 cost units against the sequential aggregate's 175,482. [Grouped-count timings and full plans](test/review/evidence/481f876/group-cost/results.json). Both document predicates have the same default and control scan route, with all heap buffers hit and zero shared-buffer reads in the captured samples. [Multikey timings and full plans](test/review/evidence/481f876/multikey-cost/results.json).

These close the measured planner findings; they do not prove that every cardinality, cache size or operator combination is optimally costed. The 20k-group rejection regression also passes, so the improvement is not a blanket forced preference for LionCount.

## Validation and scope

- **17/17 SQL regressions and 6/6 isolation tests pass** on a fresh private assertion-enabled cluster. [Test log](test/review/evidence/481f876/installcheck.txt).
- **Both unit suites pass:** 660,060 container checks and 290,234 sparse checks, with zero failures. [Unit log](test/review/evidence/481f876/unit.txt).
- **Crash-recovery/hot-standby harness passes:** eight crashes, including two during VACUUM; 511,213 generic WAL records replayed; standby visibility checks, preserved/cancelled snapshots with feedback on/off, and promotion. Runtime 62 seconds. [Summary](test/review/evidence/481f876/recovery-summary.txt), [compressed full log](test/review/evidence/481f876/recovery-full.txt.gz).
- The previously untested recovery axis now has fresh automated evidence. No new sanitizer run, sustained write soak, true cold-device campaign or alternate PostgreSQL-version testing was performed. Fixed bucket directories, historical-entry retention and hot-bucket contention remain architectural tradeoffs, not newly closed performance guarantees. The full comparison and stress suites were not rerun.
- Review reproducers now use `pg_lion`/`lion`; old evidence remains unchanged. The grouped-cost reproducer now includes an explicitly forced sequential timing control, and partition results are checked with bidirectional `EXCEPT ALL`.

## Current quick benchmark

The default **1M/5M scalar + 200k-document** run completed in **231.4 seconds (3 minutes 51 seconds)**, including private-cluster initialization and shutdown: **160 exact-result checks, 320 timings, zero errors**. The matrix/cross-portfolio [audit passes](bench/results/quick/2026-09-21-481f876/AUDIT.json), and artifact checksums verify. It uses only B-tree, GIN, `roaring` and `roaring_bitmap`; these last two labels now refer to the `lion` access method with count pushdown enabled/disabled. All other test clusters were stopped before timing.

[HTML tables](bench/results/quick/2026-09-21-481f876/index.html), [Markdown report](bench/results/quick/2026-09-21-481f876/REPORT.md), [CSV](bench/results/quick/2026-09-21-481f876/summary.csv). This is a fresh baseline at `481f876`. Automatic comparison with the historical `2026-09-21-scales` run is rejected because the rename changes the workload hash; that compatibility check was not bypassed. Two samples per configuration provide progress signals rather than stable performance bounds. One bucket-directory growth warning for the 5M `ix_c20k` index was logged; the run and correctness checks completed successfully.

---

# Historical follow-up at `7db8f11`

Everything below describes the earlier commit and retains its original findings, timing evidence and validation limits. The current status above supersedes those conclusions.

> Note: this document predates the rename to **pg_lion** (2026-09-21). Names map as: extension `roaring_index` -> `pg_lion`, access method `roaring` -> `lion`, functions `roaring_index_*` -> `lion_index_*`, GUC `roaring_index.*` -> `pg_lion.*`, C prefix `rbi_`/`RBI` -> `lion_`/`LION`/`Lion`, files `src/rbi_*.c` -> `src/lion_*.c`.

# Follow-up code, architecture, and performance review — 2026-09-21

Reviewed commit: `7db8f11` (`Fix the eight findings of the 2026-09-20 codebase review`). The extension fixes are substantially effective, and the expanded regression suite passes. **Four P2 findings remain: two reliability gaps and two measured planner-performance issues.** The review did not reproduce a remaining P1 in the previously affected paths; passing these checks does not establish correctness under every SQL, recovery, or concurrency scenario.

## Remaining findings

### [P2] Partitioned grouping relies on an estimate instead of enforcing its memory budget

Locations: [planner guard](src/rbi_customscan.c#L1733), [hash construction](src/rbi_customscan.c#L2118), [unbounded insertion](src/rbi_customscan.c#L2722).

The new guard declines a partitioned grouping path when estimated groups times estimated entry size exceeds `get_hash_memory_limit()`. The executor still builds a `TupleHashTable` containing every distinct group across partitions. `rbi_hash_add_group()` inserts each new group without tracking a runtime byte limit, spilling, or stopping. A plan-time estimate is useful admission control but is not a resource bound: inherited partition statistics can be stale, and real key widths/cardinality can exceed estimates.

**Reproduced:** create two partitions with 20,000 rows and ten distinct keys, analyze the parent, then insert 20,000 new distinct keys and vacuum without analyzing. With `work_mem='64kB'`, `hash_mem_multiplier=1`, and sequential scans discouraged, EXPLAIN ANALYZE reports:

```text
Custom Scan (RoaringCount) ... rows=10 ... actual rows=20010
```

The ordinary aggregate on the same input reports `Batches: 537`, `Memory Usage: 112kB`, and `Disk Usage: 960kB`. The custom implementation has no corresponding spill path. A correlated lateral inspection of `pg_backend_memory_contexts`, after the custom node emits its first group, measures **1,582,144 allocated bytes and 1,489,680 used bytes** in its three named group-hash contexts. That is about **1.51 MiB allocated / 1.42 MiB used**, over 22 times the 64 KiB budget. This is an observed live allocation, not an allocator-peak measurement or whole-backend RSS. The reproduction is deliberately small and did not attempt to exhaust memory or crash the server. The output is correct in this case; the issue is unbounded resource consumption on a larger instance.

Add runtime accounting with a safe spill/merge design, or fail clearly once a conservative actual budget is exceeded. A fallback after partial aggregation needs a deliberate rescan/output protocol; it cannot simply swap plans after producing rows. Add a regression with underestimated inherited statistics in addition to the existing accurately estimated low-memory rejection test.

Reproducer: [test/review/partition_estimate.sql](test/review/partition_estimate.sql). Captured output: [test/review/evidence/partition-estimate.txt](test/review/evidence/partition-estimate.txt). This is the remaining partition-hash portion of original finding 6. The separate dirty-TID recheck buffer now flushes bounded batches and preserves heap-block grouping.

### [P2] Benchmark cleanup can mutate an unverified endpoint after startup failure

Location: [bench_cleanup](bench/lib.sh#L27), called by the EXIT trap installed in [bench_start_cluster](bench/lib.sh#L40).

The launchers now reject an unsuccessful `pg_ctl start` and verify `SHOW data_directory` before the main benchmark body. However, `BENCH_PREFIX` is assigned and the EXIT trap is installed before either check. Cleanup calls `bench_restore_indexes()` whenever the prefix is nonempty; it does not require `BENCH_STARTED=1` or verified endpoint ownership. Consequently, failed startup, an already-running requested directory, or a failed identity check can still send this SQL to the configured socket/port:

```sql
UPDATE pg_index SET indisvalid = true
WHERE indexrelid::regclass::text LIKE 'fact\_%' AND NOT indisvalid;
```

If another cluster owns the endpoint, this promotes its matching invalid indexes to valid. A genuinely incomplete index could then be used for queries. Even on the correct cluster, the helper restores all matching invalid indexes rather than only indexes whose original valid state this run changed.

**Reproduced without touching a database:** fake `pg_ctl` always fails and fake `psql` records its arguments. The launcher exits 1 and the capture still contains the catalog UPDATE. The earlier `DROP EXTENSION ... CASCADE` path is now gated correctly; the remaining failure is specifically in cleanup.

Only run cleanup SQL after both successful startup and endpoint verification, and restore the exact original state of indexes modified by this run. Prefer separate tables/portfolios over editing system catalogs. The new comprehensive suite uses a unique private socket and a freshly initialized cluster, refuses output-directory overwrite, verifies its connection, and compares physically separate portfolios without catalog validity changes. It does **not** fix or silently replace the older launchers.

Reproducer: [test/review/benchmark_cleanup.py](test/review/benchmark_cleanup.py). Captured output: [test/review/evidence/cleanup-repro.txt](test/review/evidence/cleanup-repro.txt). This is the remaining part of original finding 8.

### [P2] Grouped-count costing still rejects a much faster cached recheck path

Location: [rbi_cost_count_rel](src/rbi_customscan.c#L1080).

The corrected single-count estimate bounds visits by dirty heap pages. For grouping, however, it estimates `min(numgroups * dirty_pages, recheck_tids)` page visits and charges **`random_page_cost` for every visit**, without accounting for reuse of the same dirty-page working set across groups. This can give a small resident working set the price of hundreds of thousands of random disk fetches.

**Measured in the new benchmark:** on five million rows with 200 groups, updating the first 5% of rows leaves 91,345 of 100,407 heap pages all-visible. The SQL is `SELECT c200,count(*) FROM fact GROUP BY c200`, with 64 MB work_mem and 512 MB shared buffers. Across ten randomized timing rounds per mode:

| Planner mode | Actual route | Execution median |
| --- | --- | --- |
| Default | Sequential scan + HashAggregate | 1,173.510 ms |
| Sequential scans discouraged | RoaringCount | 148.085 ms |

Both routes passed exact result-multiset checks. The selected sample plans estimate the ordinary aggregate at **175,511 cost units** and RoaringCount at **1,815,739**. Yet the RoaringCount sample reports **418,819 shared-buffer hits and zero reads**, including 415,884 heap-block rechecks: repeated buffer visits are real work, but they are not repeated random physical reads. Only 9,062 physical heap pages lack the VM bit in this state, about 71 MiB, well within the configured buffers. The default choice loses about **7.9×** in this measured workload. After the additional scattered updates, the corresponding medians are **892.878 ms versus 132.675 ms**, about **6.7×**.

Account for distinct dirty pages and expected cache reuse across group passes, separately from repeated buffer/tuple CPU work. Validate the adjustment across cache footprints and visibility states; a blanket preference for the custom path would hide the opposite failure on workloads that really are expensive to recheck. This is a performance finding, not a result-correctness failure, and does not invalidate the improvement to the single-count bound.

Evidence: [selected full plans](test/review/evidence/group-cost-plans.json), the main benchmark's `group_c200` samples, and [focused reproducer](test/review/group_cost.py). This narrows the remaining part of original finding 5 to repeated grouping rechecks.

An independent run on a fresh cluster, with only the `c200` index and randomized interleaving of both planner modes, reproduced **941.099 ms default versus 134.386 ms RoaringCount** across ten rounds, approximately **7.0×**. Both results matched the sequential reference. [Focused timing and full-plan evidence](test/review/evidence/group-cost-focused.json).

### [P2] Generic index costing treats full multikey fallback scans as selective lookups

Locations: [generic cost estimator](src/rbi_am.c#L197), [multikey fallback classification](src/rbi_multikey.c#L443), [full-index emission](src/rbi_scan.c#L586).

Phrase, prefix, NOT, and weighted tsqueries cannot all be answered by the stored exact lexeme memberships. The extractor conservatively returns `RBI_QMODE_ALL`, and the bitmap scan walks all key postings and sends candidates for heap recheck. That preserves correctness. However, `rbicostestimate()` passes an empty `GenericCosts` to `genericcostestimate()` without accounting for this mode. It prices the index access using the predicate's estimated matches even though candidate generation scans the entire index. Full-index traversal, duplicate posting emissions, and broad heap rechecking are consequently underpriced.

**Independently reproduced:** on a vacuumed 200,000-document table with one roaring tsvector index and a heap that fits in shared buffers, compare the normal planner with index/bitmap scans disabled. Ten randomized interleaved rounds give:

| Predicate | Default bitmap plan median | Sequential median | Default-plan penalty |
| --- | ---: | ---: | ---: |
| `tsv @@ 'common <-> w1'::tsquery` | 68.560 ms | 31.762 ms | 2.16× |
| `tsv @@ 'rare12:*'::tsquery` | 62.291 ms | 23.834 ms | 2.61× |

Both routes pass exact-result comparisons. The prefix plan estimates total cost **192.15**, versus **9,156.14** for the sequential alternative. It estimates 50 index rows but emits **1,176,746 posting TIDs before bitmap deduplication**, rechecking 198,020 visible candidates across 6,628 heap blocks. The selected plans have zero shared-buffer reads, so the measured penalty is not a cold-cache comparison. The phrase plan has the same full posting traversal, despite a different output selectivity. Native GIN's much faster phrase/prefix timings in the main benchmark further show why exact-lexeme count performance should not be generalized to these operators.

Cost full-scan extraction modes using their candidate coverage and index traversal work, independently of the final predicate's output selectivity. Handle unknown parameter values conservatively. More selective safe candidate extraction would be an additional optimization, not a substitute for charging the current fallback honestly. This is a new performance finding, distinct from the grouped-count cache-cost issue above; no incorrect result was observed.

Reproducer: [test/review/multikey_cost.py](test/review/multikey_cost.py). [Timings, SQL, and full plans](test/review/evidence/multikey-cost-focused.json).

## Status of the original review

| Original finding | Current assessment | Evidence / practical limit |
| --- | --- | --- |
| 1. Reserved entry identity lost on spill | Addressed | Spill now preserves `RBI_ENTRY_RESERVED`; expanded empty-array/tsvector/null regressions pass. Existing indexes damaged by older code still require rebuilding. |
| 2. Direct SQL count snapshot eligibility | Addressed | Shared eligibility helper checks catalog state and `indcheckxmin`; count and verifier use it. New `count_checkxmin` isolation test passes. |
| 3. Incompatible grouping equality | Addressed | Driving index equality must match grouping equality; incompatible-opclass regression passes. |
| 4. Historical/nonvisible group representative | Addressed | Value-producing pushdown requires equality-compatible, representation-safe types. Unsafe cases use ordinary aggregation; citext regression passes. |
| 5. Dirty-page cost overestimate | Partially addressed | Single-count page bound is corrected and the existing plan-choice regression passes. Grouped rechecks still charge repeated cached visits as random I/O; benchmark evidence shows a 7–8× default-plan penalty. |
| 6. Unbounded dirty rechecks / partition group hash | Partially addressed | Dirty rechecks are batch-bounded; partition grouping only checks estimated memory. See finding above. |
| 7. Cached access-method OID across recreation | Addressed | Planner resolves the AM identity through catalog lookup; drop/recreate regression passes. |
| 8. Benchmark endpoint safety | Partially addressed | Main body checks startup/identity; cleanup bypasses them. See finding above. |

The fix also adds a version/shape marker to the custom scan's positional private data and sorts multi-key extraction by hash before deduplication. These address two architecture/performance recommendations from the original review. Equality comparisons remain necessary within equal-hash runs; pathological hash collisions can still cost quadratic comparisons, so the ordinary-case improvement should not be described as a universal worst-case bound.

## Architecture and performance reassessment

The container and sparse-bitmap code remains cleanly separated from PostgreSQL storage. Centralized snapshot eligibility and explicit grouping contracts improve the correctness boundary between index membership and SQL execution. The new private-data marker helps detect planner/executor format disagreement; the representation is still positional and the large planner/count modules remain candidates for focused extraction after the resource issue is resolved.

The review's storage tradeoffs remain: the bucket directory does not grow after build, inserts serialize on bucket locks, spilled posting containers occupy owned pages, and VACUUM does not recycle all historical storage. None is by itself a newly reproduced corruption bug. They imply that count speed, small static index size, and sustained write performance must be evaluated separately. A bulk-built index can behave very differently from one created empty and grown by inserts.

The completed supplement quantifies those tradeoffs. For 100,000 unique keys, bulk build uses 847 buckets while an index created empty stays at 64; the 100-key IN median rises from 0.104 to 0.321 ms in the latter construction route. Five changing-key/VACUUM cycles leave 600,000 historical entries for 100,000 live TIDs, with the two-index portfolio growing from 7.594 to 34.133 MiB; REINDEX reduces it to 7.672 MiB. Other methods also retain storage, so this is not a claim that roaring alone bloats. In the short eight-client insert burst, roaring reaches 844.8 transactions/s versus B-tree's 1,629.1, with 100 rows per transaction. See the [results guide](bench/COMPARISON.md) for controls, limits, and comparisons with all families.

The new [benchmark suite](bench/comprehensive/README.md) compares sequential scans, B-tree, a composite/covering B-tree portfolio, hash, scalar GIN/GiST, BRIN, and roaring with count pushdown on/off. It covers selectivity, skew, locality, NULLs, IN lists, intersections, unions, heap fetches, ranges, ordering, grouping, arrays, full-text expressions, visibility transitions, low work_mem, build cost/size, maintenance/WAL, and concurrent reads. Unsupported operator combinations and planner fallbacks are identified explicitly. Shared-buffer-cold runs retain the OS cache. Raw plans and successful exact-result checks accompany timings.

A separate supplement compares bulk build with growth from an empty index, five rounds of changing every unique key followed by VACUUM at fixed live row count, fully dirty counts at three work_mem settings, and short 1/4/8-client insert bursts. It records roaring bucket/entry statistics and GIN's post-burst VACUUM cost. Direct SQL count diagnostics ensure the bounded dirty-recheck engine is exercised even when the normal planner prefers a different route. These are controlled experiments, not a sustained mixed-workload endurance test.

This suite does not establish production readiness: sustained multi-client writes, crash/recovery, standby replay, alternate PostgreSQL versions, and true cold-device IO are separate validation axes. Read the benchmark's measured results and limitations before drawing a workload-wide conclusion. The historical 20-million-row benchmark has not been rerun in this follow-up.

## Validation

- Built the fixed extension from a clean source copy against the assertion-enabled PostgreSQL 20devel installation; **17/17 SQL regression tests and 6/6 isolation tests passed**. [Captured test output](test/review/evidence/installcheck.txt).
- Built another clean copy against PostgreSQL 20devel configured with `CFLAGS=-O2`, without assertions, for performance measurements. The review/test cluster was stopped before the measured benchmark began.
- Reproduced both reliability gaps with the checked-in bounded SQL and stubbed shell scripts. Independent owned-cluster experiments confirm both planner-cost findings; full plans and randomized timing rounds are checked in. No unrelated database was used or modified.
- Executed the entire new benchmark matrix at 10,000 scalar rows / 2,000 documents as a smoke test, including maintenance, concurrent clients, and owned-server restarts: **zero mismatches and zero query errors**. This validates the harness, not meaningful production performance.
- Completed the main comparison at one/five million scalar rows and 200,000 documents: **32,768 timed observations and 3,266 successful exact-result checks**, with zero query errors or result mismatches. Five native hash index builds and one hash REINDEX hit the common 60-second statement limit; partial portfolios and four dependent unmeasured post-maintenance configurations are explicit. [Artifact/matrix audit](bench/results/2026-09-20-comparison/AUDIT.json).
- Completed the supplement: **2,500 timed queries, 250 exact-result checks, 21 writer bursts with row-count checks, and 42 indexed post-write checks**, all passing. [Supplement audit](bench/results/2026-09-21-stress/AUDIT.json). Both suites retain raw observations, SQL, full query plans, settings, reports, CSVs, and checksums.
- The original standalone container/sparse unit and sanitizer results remain historical evidence; these unchanged modules were not rerun under sanitizers in this follow-up. No fresh crash/recovery or standby test campaign was performed.

The source review remains separate from the benchmark artifacts. No extension implementation fixes are included in this follow-up; the new code is the benchmark harness and safe reproductions.
