# Reproducible PostgreSQL index comparison

For routine development, use the [quick progress benchmark](../QUICK.md). It keeps B-tree,
GIN, roaring, and roaring_bitmap at 1M and 5M rows, uses two timing rounds and a reduced
query/index matrix, and generates baseline query deltas automatically.
The latest default run at `481f876` finished in 3 minutes 51 seconds on the benchmark host.

The default **focused** suite compares **B-tree, GIN, roaring, and roaring_bitmap**. It keeps the 1M/5M scales and representative wins, parity cases, and losses, while dropping repetitive query variants and irrelevant competitors. Sequential execution remains the untimed correctness reference. `roaring_bitmap` reuses the Lion indexes with count pushdown disabled. The old exhaustive suite is available with `--profile full`.

Both profiles create a private temporary database cluster; neither changes `pg_index.indisvalid` or connects to a user-supplied database endpoint. The quick benchmark remains unchanged for frequent progress checks; focused adds high-cardinality/skew cases, low-memory diagnostics, and a third timing round.

Start with the [results and interpretation](../COMPARISON.md). The full measured report is [searchable HTML](../results/2026-09-20-comparison/index.html) or [complete Markdown](../results/2026-09-20-comparison/REPORT.md). The [follow-up review](../../FOLLOWUP_REVIEW.md) tracks the remaining code findings. Historical measurements in the root README use different datasets/builds and should not be merged into this run.

The [growth/churn/write supplement](../results/2026-09-21-stress/REPORT.md) additionally compares empty-index ingestion, five changing-key/VACUUM cycles at constant live cardinality, fully dirty low-memory counts, custom/generic prepared plans, and 1/4/8-client insert bursts with indexed result checks. Its [HTML tables and plots](../results/2026-09-21-stress/index.html) and raw records are separate from the main eight-index portfolio.

## Run

Requirements: Linux, Python 3.9+, PostgreSQL binaries and `libpq.so` under one installation prefix, and installed `pg_lion`, `btree_gin`, and `pg_visibility` extensions. `btree_gist` is needed only when GiST is explicitly selected or with the full profile. Use a release PostgreSQL build for representative performance. The exact server version, configure flags, machine information, settings, seed, SQL, and source commit are saved with each run. Build/install the extension against that prefix first using the repository's normal PGXS workflow. PostgreSQL requires an unprivileged OS user.

```sh
python3 bench/comprehensive/run.py \
  --prefix /path/to/postgresql \
  --output bench/results/my-comparison
python3 bench/comprehensive/report.py bench/results/my-comparison
python3 bench/comprehensive/audit.py bench/results/my-comparison
```

The runner needs only Python's standard library and the supplied PostgreSQL installation's libpq. Report charts require Matplotlib (`python3 -m pip install matplotlib` if it is not already available). The generated HTML is standalone and works locally without a web server or external scripts; the SVG figures are separate exportable artifacts as well.

The focused default uses two scalar sizes (1,000,000 and 5,000,000 rows), 200,000 documents, **three timing rounds, one warmup, and one build per index**. It schedules **858 timings and 46 index builds**, versus **33,888 timings and 372 builds** in the current full profile: about **97% fewer timings and 88% fewer builds**. These are workload counts, not a measured wall-clock speedup; loads, correctness queries, and writes still cost time. Runtime at the default scales has not yet been measured for this profile.

To reproduce the broader matrix deliberately:

```sh
python3 bench/comprehensive/run.py --profile full \
  --prefix /path/to/postgresql --output bench/results/full-comparison
```

Explicit options override profile defaults. For example, `--repeats 7` improves sampling without restoring irrelevant index types, and `--cold-repeats 1` adds a small restart experiment. Add `--preload` to measure Lion's custom WAL resource manager; leave it off to measure generic WAL. Keep that choice fixed across comparisons.

For a full-path smoke test:

```sh
python3 bench/comprehensive/run.py \
  --prefix /path/to/postgresql \
  --output /tmp/lion-comparison-smoke \
  --rows 10000 --documents 2000 \
  --repeats 1 --warmups 0 --build-repeats 1 \
  --cold-repeats 0 --duration 0
python3 bench/comprehensive/report.py /tmp/lion-comparison-smoke
python3 bench/comprehensive/audit.py /tmp/lion-comparison-smoke
```

Use `--help` for subsets. `--duration 0` disables concurrency, `--cold-repeats 0` disables restarts, and `--no-maintenance` skips mutations after the read matrix. Output directories must be new unless `--resume` is explicitly supplied. A normal completion or Python exception stops the owned cluster and removes its temporary data; `--keep-cluster` retains stopped data for inspection. A hard process kill or host crash can leave an orphan under `/tmp/lion-comparison-*`; inspect its `postmaster.pid` and use the matching installation's `pg_ctl -D ... stop` before removing it. The directory is private and TCP listening is disabled.

To resume at a completed portfolio boundary, rerun the same command with `--resume`. The source commit, profile, and measurement arguments must match. Old runs without a profile are treated as full runs. Completed portfolios are retained, and a fresh private cluster handles the rest; resume history is saved. A partially measured portfolio requires a separate run/output directory rather than silently mixing repeated samples. Build statements have the same 60-second statement limit as queries. A failed index is recorded, omitted, and not retried in later build rounds. Its portfolio is labeled **partial** in reports, and query plans show whether surviving indexes or a fallback answered each case. A partial portfolio has no claimed complete build time, and its maintenance/storage costs should not be compared as a complete portfolio.

Maintenance statements also have the 60-second limit. If one fails, its attempt time/WAL are recorded as a failure, later dependent maintenance work is omitted, and the four post-maintenance query configurations are explicitly marked unmeasured. The completed read matrix remains usable. Resume also recognizes an older runner that stopped at this precise boundary; it retains those completed reads and records the same omissions. The audit requires a recorded maintenance failure for every such skipped configuration. A timeout is not reported as a successful operation duration, and omitted checks are not reported as passes.

Run the supplement after the main suite, so the two do not compete for resources:

```sh
python3 bench/comprehensive/stress.py \
  --prefix /path/to/postgresql \
  --output bench/results/my-stress
python3 bench/comprehensive/stress_report.py bench/results/my-stress
python3 bench/comprehensive/audit.py bench/results/my-stress
```

The supplement now defaults to B-tree/GIN/Lion, three timing rounds, two churn cycles, and 1/4-client write bursts. It remains a separate, occasional run. To restore its historical coverage, pass `--families seq btree hash gin gist brin roaring --repeats 10 --cycles 5 --clients 1 4 8`.

## Matrix

| Dimension | Focused default | Full opt-in |
| --- | --- | --- |
| Methods | B-tree, GIN, roaring, roaring_bitmap | Adds seq, tuned B-tree, hash, GiST, BRIN |
| Scale | 1M/5M scalar rows; 200k documents | Same |
| Scalar queries | 19: dense/medium/selective/rare equality, hot/cold skew, IN 10/1000, AND/OR, NULL, fetch, range, ordered LIMIT, grouping | 44 cases including extra constants, IN lengths, clustered keys and correctness edge cases |
| Documents | 10: array containment/overlap, common/rare terms, AND/compound query, phrase/prefix fallback, fetch | 21 cases |
| Planner | Default; forced-index diagnostics only for low memory and dirty grouping | Default and forced-index for every clean/dirty case |
| Visibility | Clean; four cases after scattered payload updates, plus grouping diagnostic | Also clustered dirty state; all stress cases in both dirty states |
| Memory | Four selected scalar cases at 64 kB; otherwise 64 MB | Same |
| Repetitions | 3 timed rounds, 1 warmup, 1 build | 10 rounds, 2 warmups, 3 builds |
| Indexes | Seven scalar indexes; tags/tsv for documents | Eight scalar indexes; tags/tsv/grp for documents, subject to family support |
| Maintenance | INSERT, indexed UPDATE, DELETE, VACUUM, four post-write checks | Adds REINDEX |
| Cold/concurrency | Off | Three restart trials per selected query; 5-second 1/4/8-client reads |
| Correctness | Exact result-multiset SHA-256 against forced sequential execution for every warm configuration | Same |

The focused suite intentionally drops multiple equality constants with the same selectivity, redundant IN lengths, duplicate/NULL syntax variants, extra range widths, and fallback-only document cases. Those semantics belong primarily in regression tests; the full benchmark remains useful for occasional investigations. Range, ordered LIMIT, heap fetch, and phrase/prefix queries stay because they expose practical losses or fallbacks. High-cardinality equality and skew stay because low-cardinality wins alone are not representative.

Identical full-width synthetic heaps are regenerated for each portfolio, using independent fixed seeds for each column. The focused profile omits the unused `clustered` and document `grp` indexes. Its storage/write costs therefore differ from the full portfolio. Its scattered dirty state starts from a clean heap; the full profile first dirties a clustered region. Do not merge timings or portfolio costs across profiles. The saved query matrix drives coverage auditing, including for historical runs.

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
