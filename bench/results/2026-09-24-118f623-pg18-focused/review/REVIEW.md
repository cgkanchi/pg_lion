# Review of 118f623 — 2026-09-24

Source was reviewed without fixes. Two reproducible gaps remain in the newly introduced comparator-change guard. Both require an owner/administrator to change a custom comparison function after building an index. These are integrity/guard-coverage issues, not a demonstrated privilege escalation or a failure under the built-in operator classes.

## P2: SQL-standard comparison bodies are absent from the recorded identity

`src/lion_pages.c:768–788`, `lion_proc_ident()`, hashes only `prosrc` and `probin`. A SQL-standard `RETURN` function stores its parsed body in `prosqlbody`, with an empty `prosrc`. Replacing an ascending comparator with a descending body leaves the fingerprint unchanged. Even after reconnecting, the existing index is accepted and `lion_index_count(idx, 7)` returns **0**, although sequential evaluation finds **10** rows.

Reproduction: `sql-body.sql`; observed results: `sql-body.json`. Account for parsed SQL bodies, or explicitly reject unsupported comparator identities. A stored-tree hash must also handle catalog OIDs across pg_upgrade rather than assuming a raw serialized tree is stable.

## P2: Cached index state bypasses comparator-change validation

`src/lion_pages.c:891–892`, `lion_get_index_state()`, returns `rd_amcache` without validating the current comparison identity. A backend that has already opened the index continues after an ordinary string-body `CREATE OR REPLACE FUNCTION` changes the comparison. The new SQL body executes against the old directory order and again produces **0 instead of 10**, without the promised REINDEX error. This reproduces even with `prosrc` changing, so adding `prosqlbody` to the fingerprint alone does not fix it.

Reproduction: `cached-state.sql`; observed results: `cached-state.json`. Invalidate/revalidate cached order state when relevant function definitions change; regression coverage needs an already-open backend as well as a fresh one.

## Validation

- Archived exact commit; independent PostgreSQL 18.6 release and PostgreSQL 20devel assertion builds succeeded with WERROR=1.
- All 32 SQL regressions passed on both versions. Six default isolation cases passed on PG18; five targeted cases passed on PG20, including the injection-point range/VACUUM race.
- 768 additional exact-result comparisons passed for clean/dirty data, 64MB/64kB work_mem, count pushdown on/off, ranges, range ANY, NULLs, NaNs, mixed integer widths, multiple columns, and grouped counts. 120 examined plans contained LionCount.
- Clang static analysis completed across extension translation units. Reviewed diagnostics did not establish an additional defect: buffer-handling advisories and conservative bounds/null-path reports require PostgreSQL allocation/offset invariants. Raw logs are in `static/`; this is not a claim of a warning-free analyzer run.
- No new sanitizer, crash-recovery, or endurance campaign in this pass. Normal benchmark correctness checks do not cover comparator-changing DDL.

## Focused PG18 benchmark

The 1M/5M scalar and 200k-document suite completed in 393.99 seconds: 858 timing samples, 286 exact-result checks, 72 cross-portfolio result groups, zero errors, audit passed. Three measured rounds per configuration; warmed caches, no cold-device or concurrency campaign. Extension built from the archived commit on PostgreSQL 18.6 release with custom rmgr preloaded. No other test/analyzer processes launched by this review were running during timing.

| 5M rows, clean, default planner | Lion ms | B-tree ms | GIN portfolio ms | Lion speedup vs B-tree / GIN |
| --- | ---: | ---: | ---: | ---: |
| Dense equality count | 1.744 | 157.028 | 665.041 | 90.0× / 381.3× |
| Medium equality count | 0.095 | 1.612 | 18.097 | 17.0× / 190.5× |
| Two-predicate intersection count | 1.778 | 16.380 | 171.158 | 9.2× / 96.3× |
| Group into 200 buckets | 17.074 | 405.735 | 1045.313 | 23.8× / 61.2× |
| Range count | 0.663 | 1.629 | 428.312 | 2.5× / 646.0× |

The range count was 712.504 ms at 7cb7711, using a sequential fallback; it now uses LionCount. With pushdown disabled, the new Lion range bitmap path takes 152.083 ms, so this is specifically an aggregate-pushdown win, not a general claim that range retrieval beats B-tree. GIN's grouped query uses a sequential fallback.

Remaining trade-offs: medium row fetch is 16.280 ms for Lion versus 14.828 ms for B-tree; ordered LIMIT remains a sequential fallback at 857.024 ms versus a B-tree index-only scan at 0.021 ms. Across comparable configurations whose previous median was at least 1 ms, median new/old time ratios are 0.935 for Lion, 0.950 for B-tree and 0.948 for GIN. Most small changes therefore resemble common run variation; this short run does not establish small improvements as causal.

Reports: ../REPORT.md and ../index.html; all per-case changes: ../BASELINE_COMPARISON.json.
