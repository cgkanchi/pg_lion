# Frequent progress benchmark

Use [quick.py](quick.py) after building and installing the current extension. The `quick-v2` default covers **both 1,000,000 and 5,000,000 scalar rows**, plus **200,000 documents**, with two timing rounds. The [current measured run at `481f876`](results/quick/2026-09-21-481f876/REPORT.md) finished in **231.4 seconds (3 minutes 51 seconds)**, including cluster initialization and shutdown, with **320 timings and 160 exact-result checks**, all passing. This used a freshly rebuilt release extension. Runtime depends on hardware/build settings; the measurement deadline is five minutes.

The [pre-rename 1M/5M run](results/quick/2026-09-21-scales/REPORT.md) finished in 268.1 seconds and remains historical evidence. Renaming the access method in the SQL changes the workload hash, so automatic comparison with that run is rejected. Use the current run as a new baseline only with matching environment/workload settings. The `roaring` and `roaring_bitmap` portfolio labels are retained; both now use the `lion` access method.

The earlier [500k/50k baseline](results/quick/2026-09-21-baseline/REPORT.md) remains a historical `quick-v1` run. The larger profile requires a new baseline; the runner rejects comparisons between these profiles.

Only **B-tree, GIN, roaring, and roaring_bitmap** are timed. Sequential execution remains an untimed correctness reference. `roaring_bitmap` reuses the lion indexes with count pushdown disabled. Document queries compare GIN and roaring because B-tree has no matching array/full-text index in this workload.

## Run after each change

Requirements: Python 3.9+ on Linux, a PostgreSQL installation containing `libpq`, and installed `pg_lion`, `btree_gin`, and `pg_visibility`. No Python packages or charting dependencies are required. The runner creates a private temporary cluster and stops/removes it afterward. It never connects to an existing database.

For the repository's local PostgreSQL installation:

```sh
make -j4 PG_CONFIG="$PWD/.local/pg/bin/pg_config"
make PG_CONFIG="$PWD/.local/pg/bin/pg_config" install
python3 bench/quick.py --prefix "$PWD/.local/pg"
```

For another installation, pass its root directory with `--prefix /path/to/postgresql` and build/install with the corresponding `bin/pg_config`. The benchmark does not rebuild the extension automatically. Use the same server build and an otherwise idle machine for comparisons; release and assertion-enabled builds should not be mixed.

Output defaults to `bench/results/quick/<UTC timestamp>-<commit>`. Automatically named runs are ignored by Git but remain on disk for comparisons; explicitly named baselines can be committed. Existing output directories are never overwritten. To establish and compare a baseline:

```sh
python3 bench/quick.py --prefix "$PWD/.local/pg" \
  --output bench/results/quick/before

# Make the change, rebuild, and install the extension, then:
python3 bench/quick.py --prefix "$PWD/.local/pg" \
  --baseline bench/results/quick/before
```

Open the printed `index.html`, `REPORT.md`, or `summary.csv`. Query comparisons show the previous median, percentage change (**positive means slower**), and old/new scan routes. Source commits and installed extension binary hashes are recorded separately. A changed commit or extension binary is expected; a different workload, seed, repetition count, server binary/configuration, or recorded machine characteristics is rejected as an incompatible baseline. The checked-in release-build baseline is illustrative; establish a local baseline for your own installation.

You can regenerate or compare saved results without starting PostgreSQL:

```sh
python3 bench/quick.py --report-only bench/results/quick/after \
  --baseline bench/results/quick/before
```

## Fixed small matrix

| Dimension | Quick default |
| --- | --- |
| Scalar datasets | 1,000,000 and 5,000,000 rows; same full-width deterministic heaps as the full suite |
| Documents | 200,000 rows |
| Repetitions | Two timed rounds, one warmup, one build per index |
| Clean scalar reads | Ten cases at each scale: dense/selective equality, IN, AND, NULL, heap fetch, range, and grouping |
| Dirty reads | Four cases after scattered payload updates; an additional forced-index grouping diagnostic |
| Writes | One 1% INSERT, one 1% indexed UPDATE, and VACUUM per scalar portfolio and scale; time, WAL, size, and two post-maintenance checks |
| Documents | Eight array/full-text cases, including phrase/prefix fallbacks and payload fetch |
| Planner | Defaults ordinarily; actual routes recorded, including sequential fallbacks |
| Correctness | Exact result-multiset hashes against sequential execution, plus agreement across portfolios |

Scalar portfolios build only the five indexes used by this matrix: `c2`, `c20`, `c200`, `c20k`, and `nullable`. Document portfolios build `tags` and `tsv` indexes. Build/write/storage costs therefore describe these smaller portfolios, not the full suite's eight scalar indexes. GIN scalar support comes from `btree_gin`. The profile omits tuned B-tree, hash, GiST, BRIN, timed sequential baselines, low-memory diagnostics, ordered LIMIT, cold-cache restarts, concurrency, REINDEX, and extended churn. Use the [comprehensive suite](comprehensive/README.md) when those dimensions matter.

Shared buffers are 512 MB, ordinary work_mem is 64 MB, durability is enabled, and parallel query/JIT/autovacuum are disabled. Explicit vacuum and mutations control visibility. Timing is server EXPLAIN ANALYZE execution time with node timing off; planning is reported separately. Family order and timing rounds use a fixed random seed. Warm means no deliberate cache eviction, not an assurance that every page is resident.

## Limits and artifacts

The default **300-second measurement deadline** aborts an overlong run. Each statement also has a **60-second timeout**, allowing larger table loads and index builds. Cancellation and server cleanup can add time beyond the deadline. Failed, incomplete, or mismatching runs exit unsuccessfully and cannot become comparison baselines. Timing changes are not automatic pass/fail regressions: two samples are intentionally small, their median is their midpoint, and sub-millisecond ratios are noisy. Repeat a suspected regression and use the full benchmark to investigate it.

`--rows` accepts one or more sizes: `--rows 1000000 5000000` is now the default, and `--rows 1000000` selects one scale when needed. `--documents`, `--repeats`, `--warmups`, `--seed`, and `--max-seconds` are also available. `--documents 0` omits document tests. Keep measurement settings fixed across a progress series. Changes to phase/mutation semantics should get a new profile version rather than being compared silently with `quick-v2`.

Each run includes:

- `index.html`, `REPORT.md`, `summary.csv`: compact tables, build/storage/write observations, and optional query baseline deltas.
- `samples.jsonl`, `plans.jsonl.gz`, `operations.jsonl`: raw timings, full plans, builds, mutations, visibility, and result checks.
- `metadata.json`, `queries.json`: environment, installed binary fingerprints, commit/source diff, arguments, SQL, index definitions, and declared configuration matrix.
- `AUDIT.json`: complete matrix/round/plan coverage and cross-portfolio result agreement.
- `SHA256SUMS`: checksums of the saved artifacts, refreshed when reports are regenerated.

Build and write timings are single observations, not repeated estimates. The HTML/CSV provide current build and maintenance costs; automatic baseline deltas apply to query timings, keyed by scale as well as query and state. This is a frequent progress check, not a replacement for concurrency, recovery, or the full benchmark matrix.
