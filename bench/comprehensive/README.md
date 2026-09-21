# Reproducible PostgreSQL index comparison

This suite measures the current extension against sequential scans, B-tree, a tuned B-tree portfolio, hash, GIN, GiST, and BRIN. Roaring is measured both with count pushdown and through the ordinary bitmap path. It creates its own temporary database cluster; it never changes `pg_index.indisvalid` or connects to a user-supplied database endpoint.

Start with the [results and interpretation](../COMPARISON.md). The full measured report is [searchable HTML](../results/2026-09-20-comparison/index.html) or [complete Markdown](../results/2026-09-20-comparison/REPORT.md). The [follow-up review](../../FOLLOWUP_REVIEW.md) tracks the remaining code findings. Historical measurements in the root README use different datasets/builds and should not be merged into this run.

The [growth/churn/write supplement](../results/2026-09-21-stress/REPORT.md) additionally compares empty-index ingestion, five changing-key/VACUUM cycles at constant live cardinality, fully dirty low-memory counts, custom/generic prepared plans, and 1/4/8-client insert bursts with indexed result checks. Its [HTML tables and plots](../results/2026-09-21-stress/index.html) and raw records are separate from the main eight-index portfolio.

## Run

Requirements: Linux, Python 3.9+, PostgreSQL binaries and `libpq.so` under one installation prefix, and installed `roaring_index`, `btree_gin`, `btree_gist`, and `pg_visibility` extensions. Use a release PostgreSQL build for representative performance. The exact server version, configure flags, machine information, settings, seed, SQL, and source commit are saved with each run. Build/install the extension against that prefix first using the repository's normal PGXS workflow. PostgreSQL requires an unprivileged OS user.

```sh
python3 bench/comprehensive/run.py \
  --prefix /path/to/postgresql \
  --output bench/results/my-comparison
python3 bench/comprehensive/report.py bench/results/my-comparison
python3 bench/comprehensive/audit.py bench/results/my-comparison
```

The runner needs only Python's standard library and the supplied PostgreSQL installation's libpq. Report charts require Matplotlib (`python3 -m pip install matplotlib` if it is not already available). The generated HTML is standalone and works locally without a web server or external scripts; the SVG figures are separate exportable artifacts as well.

The default run is substantial: two scalar sizes (1,000,000 and 5,000,000 rows), 200,000 documents, 10 measurement rounds, two warmups, three index-build trials, three shared-buffer-cold trials per selected query, and five-second concurrent-read runs at 1/4/8 clients. Allow ample runtime and disk space; performance depends strongly on the machine and index build costs.

For a full-path smoke test:

```sh
python3 bench/comprehensive/run.py \
  --prefix /path/to/postgresql \
  --output /tmp/rbi-comparison-smoke \
  --rows 10000 --documents 2000 \
  --repeats 1 --warmups 0 --build-repeats 1 \
  --cold-repeats 1 --duration 0.2 --clients 1 2
python3 bench/comprehensive/report.py /tmp/rbi-comparison-smoke
```

Use `--help` for subsets. `--duration 0` disables concurrency, `--cold-repeats 0` disables restarts, and `--no-maintenance` skips mutations after the read matrix. Output directories must be new unless `--resume` is explicitly supplied. A normal completion or Python exception stops the owned cluster and removes its temporary data; `--keep-cluster` retains stopped data for inspection. A hard process kill or host crash can leave an orphan under `/tmp/rbi-comparison-*`; inspect its `postmaster.pid` and use the matching installation's `pg_ctl -D ... stop` before removing it. The directory is private and TCP listening is disabled.

To resume at a completed portfolio boundary, rerun the same command with `--resume`. The source commit and measurement arguments must match. Completed portfolios are retained, and a fresh private cluster handles the rest; resume history is saved. A partially measured portfolio requires a separate run/output directory rather than silently mixing repeated samples. Build statements have the same 60-second statement limit as queries. A failed index is recorded, omitted, and not retried in later build rounds. Its portfolio is labeled **partial** in reports, and query plans show whether surviving indexes or a fallback answered each case. A partial portfolio has no claimed complete build time, and its maintenance/storage costs should not be compared as if it offered all eight indexes.

Maintenance statements also have the 60-second limit. If one fails, its attempt time/WAL are recorded as a failure, later dependent maintenance work is omitted, and the four post-maintenance query configurations are explicitly marked unmeasured. The completed read matrix remains usable. Resume also recognizes an older runner that stopped at this precise boundary; it retains those completed reads and records the same omissions. The audit requires a recorded maintenance failure for every such skipped configuration. A timeout is not reported as a successful operation duration, and omitted checks are not reported as passes.

Run the supplement after the main suite, so the two do not compete for resources:

```sh
python3 bench/comprehensive/stress.py \
  --prefix /path/to/postgresql \
  --output bench/results/my-stress
python3 bench/comprehensive/stress_report.py bench/results/my-stress
python3 bench/comprehensive/audit.py bench/results/my-stress
```

## Matrix

| Dimension | Coverage |
| --- | --- |
| Scalar methods | seq, btree, btree_tuned, hash, gin (btree_gin), gist (btree_gist), brin, roaring, roaring_bitmap |
| Integer distributions | 2 / 20 / 200 / 20,000 / up to 1,000,000 distinct values, clustered keys, 90% hot-key skew, 10% NULLs |
| Scalar queries | 43 cases: equality including absent values, IN lengths 3/10/100/1000, duplicates/NULL in IN, two/three-way AND, OR, NULL, heap fetch, range, ordered LIMIT, residual expressions, DISTINCT, grouping |
| Planner | normal defaults; `enable_seqscan=off` diagnostic, actual plan recorded |
| Visibility | vacuumed; first 5% of rows updated; additional scattered updates; measured VM coverage recorded |
| Memory | 64 MB ordinarily; 64 kB selected bitmap/count/group stress cases |
| Cache | warmed runs; selected shared-buffer-cold queries after dedicated-server restart, OS cache retained |
| Documents | 21 text-array/tsvector cases: containment, overlap, empty/NULL operands, AND/OR/compound tsquery, phrase/prefix/NOT/weight fallbacks, heap fetch, grouping |
| Build | repeated wall time and WAL per index; size per index and complete portfolio |
| Maintenance | insert, indexed UPDATE, DELETE, VACUUM, REINDEX; wall time, WAL, growth, exact post-maintenance query checks |
| Concurrent reads | closed-loop 1/4/8 clients; transactions/s, median and p95 end-to-end latency |
| Correctness | exact result-multiset SHA-256 against the forced sequential result for every warm query/configuration/state |

Identical synthetic data is regenerated for each portfolio, using independent fixed seeds for each column. A family gets the same single-column coverage; the explicitly labeled tuned B-tree adds two indexes and pays for them in size/build/write comparisons. The text-array GiST case has no applicable opclass and is a fallback, not a GiST containment performance claim. The suite does not substitute different query semantics to give an unsupported index a result.

## Outputs and interpretation

- `metadata.json`: run status, arguments, environment, compiler flags, source commit, all PostgreSQL settings.
- `queries.json`: every query and index definition; data generation is in [workloads.py](workloads.py).
- `samples.jsonl`: every timed query, execution/planning latency, actual scan route, fallback flag, buffers/temp/lossy blocks, or error.
- `plans.jsonl.gz`: complete JSON plans keyed to sample sequence numbers.
- `operations.jsonl`: exact-result checks, build times/sizes/WAL, visibility maps, maintenance observations, concurrency summaries.
- `REPORT.md`, `index.html`, `*.svg`: full table, searchable report, and standalone exportable charts.
- `summary.csv`, `indexes.csv`, `maintenance.csv`, `concurrency.csv`: spreadsheet-ready summary tables.
- `AUDIT.json`, `SHA256SUMS`: post-run checks of the declared workload coverage, timing rounds, cross-portfolio result agreement, and checksums (run `audit.py` after generating reports). The same command audits the supplement's query matrix and indexed post-write counts.

Query timing uses EXPLAIN ANALYZE with node timing off; it includes instrumentation and excludes planning/result transfer. Correctness is checked with the actual query, separately from timing. Bootstrap confidence intervals describe each query's median; low-sample cold results and p95 are approximate. Timeouts and mismatches are errors, not zero-latency results. Always inspect completion status and error count before using a report.

There is no single overall winner: use matching query, scale, visibility state, and planner mode. A sequential fallback under a named portfolio is a planner outcome rather than a measurement of that index's algorithm. Read-heavy count acceleration should not be generalized to heap fetches, range scans, or maintenance. Parallel query is disabled, so this is not a best-tuned parallel B-tree/scan comparison. The concurrency driver can become client-bound for fast queries. Maintenance currently has one observation per family/scale, not confidence intervals. The supplement covers short concurrent-write bursts and five deterministic churn cycles; long-running mixed workloads, crash/standby recovery, geometric/vector/JSON workloads, alternative opclass tuning, and genuine cold-device IO need separate studies.

Presentation was inspired by the [Biscuit benchmark](https://biscuit.readthedocs.io/en/latest/benchmark.html); this suite uses its own datasets and measurements.
