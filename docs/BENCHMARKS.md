# pg_lion benchmarks

> **These measurements are from 2026-09-24 and are out of date for full-text search and ordered
> retrieval.** They predate stored positions (exact phrase, weight and NOT queries), BM25 ranking,
> `LionOrdered`/`LionBtreeScan` and NARROW containers, so the "Where Lion loses" rows for phrases and
> ordered `LIMIT` no longer describe the current code. The count and grouping results still apply. A
> fresh run is pending.

Measured on **2026-09-24 (UTC), PostgreSQL 18.6, commit `118f623`**, using the
[focused benchmark](../bench/comprehensive/README.md): **1M and 5M scalar rows, 200k documents,
286 exact-result checks and 858 timings**, all passing. The run took **394.0 seconds
(6m 34s)** including cluster setup and shutdown; extension compilation and report generation
are separate. [Full report](../bench/results/2026-09-24-118f623-pg18-focused/REPORT.md) · [HTML](../bench/results/2026-09-24-118f623-pg18-focused/index.html) ·
[CSV](../bench/results/2026-09-24-118f623-pg18-focused/summary.csv) · [Audit](../bench/results/2026-09-24-118f623-pg18-focused/AUDIT.json).

At 5M rows, Lion's clean dense count is **90.0× faster** than B-tree,
200-group aggregation is **23.8× faster**, and the 1,000-value IN count is
**2.5× faster**. Range-count pushdown now wins too: **0.663 ms versus B-tree's
1.629 ms (2.5×)**. Tiny equality probes are at practical parity. Keep B-tree for
ordered retrieval and GIN for phrase/prefix search; dirty pages and build/write costs
still matter.

These timings include the range changes through `118f623`. The subsequent comparator-guard
fix at `07f8fcb` passed correctness verification but was not separately benchmarked.

## Setup and interpretation

Release PostgreSQL 18.6 (`-O2`, assertions off), AMD Ryzen 7 5700X3D, 16 logical CPUs, WSL2;
512 MB shared buffers and 64 MB work_mem. Durability is enabled; parallel query, JIT and autovacuum
are disabled. Lion is preloaded and uses its **custom WAL resource manager**. Each query has one
warmup and three timed rounds. Tables show median EXPLAIN execution time in **milliseconds**;
planning is separate. Three samples characterize a progress run, not statistical equivalence.
Warm means no deliberate cache eviction, not that every page fits in shared buffers.
[Environment and build provenance](../bench/results/2026-09-24-118f623-pg18-focused/metadata.json) · [Archived source](../bench/results/2026-09-24-118f623-pg18-focused/source.tar.gz).

Each scalar portfolio has **seven** single-column indexes: `c2`, `c20`, `c200`, `c20k`, `c1m`,
`skew`, and `nullable`. Documents have `tags` and `tsv` indexes. GIN uses `btree_gin` for scalars
with `fastupdate=on`. Lion and **Lion (pushdown off)** share the same `USING lion` indexes;
their artifact labels are `roaring` and `roaring_bitmap`. The tables use the default planner:
**† marks an actual sequential scan**, not index performance. The report also contains forced-index
and 64 kB work_mem diagnostics. This profile has more indexes and different query/mutation coverage
than the earlier quick runs; their portfolio costs and timings are not a like-for-like change series.

## Where Lion wins: counts and grouping

Clean measurements follow VACUUM FREEZE. Speedups are competitor median divided by Lion median,
computed before rounding; higher is better for Lion. A dash means B-tree has no matching document
measurement. Ratios include any sequential fallbacks marked †.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms | Speedup vs B-tree / GIN |
| --- | --- | --- | --- | --- | --- |
| 1M scalar | Count ~50% / clean | 30.464 | 80.748 | 0.350 | 87.0× / 230.7× |
| 1M scalar | Count ~0.5% / clean | 0.334 | 3.095 | 0.028 | 11.9× / 110.5× |
| 1M scalar | Two equality predicates / clean | 2.619 | 31.742 | 0.362 | 7.2× / 87.7× |
| 1M scalar | 1,000-value IN / clean | 3.327 | 18.954 | 1.608 | 2.1× / 11.8× |
| 1M scalar | Count per 200 groups / clean | 79.972 | 139.114 † | 3.648 | 21.9× / 38.1× |
| 1M scalar | Count `c20k BETWEEN 100 AND 199` / clean | 0.304 | 37.070 | 0.190 | 1.6× / 195.1× |
| 5M scalar | Count ~50% / clean | 157.028 | 665.041 | 1.744 | 90.0× / 381.3× |
| 5M scalar | Count ~0.5% / clean | 1.612 | 18.097 | 0.095 | 17.0× / 190.5× |
| 5M scalar | Two equality predicates / clean | 16.380 | 171.158 | 1.778 | 9.2× / 96.3× |
| 5M scalar | 1,000-value IN / clean | 17.107 | 373.333 | 6.736 | 2.5× / 55.4× |
| 5M scalar | Count per 200 groups / clean | 405.735 | 1,045.313 † | 17.074 | 23.8× / 61.2× |
| 5M scalar | Count `c20k BETWEEN 100 AND 199` / clean | 1.629 | 428.312 | 0.663 | 2.5× / 646.0× |
| 200k documents | Array contains `t1` | — | 4.045 | 0.024 | — / 168.5× |
| 200k documents | Array contains `t1` and `t17` | — | 0.820 | 0.125 | — / 6.6× |
| 200k documents | Array overlaps three tags | — | 6.449 | 0.227 | — / 28.4× |
| 200k documents | Full-text `w1 & w17` count | — | 0.952 | 0.118 | — / 8.1× |

These gains apply to the supported count shapes, not arbitrary aggregates or retrieving every match.
The same Lion indexes with count pushdown disabled show the difference:

| 5M-row query / clean | Lion ms | Lion, pushdown off ms |
| --- | --- | --- |
| Count ~50% | 1.744 | 509.001 |
| Two equality predicates | 1.778 | 46.238 |
| Count per 200 groups | 17.074 | 1,059.507 † |
| Range count | 0.663 | 152.083 |

The 5M range count fell from **712.504 ms** at `7cb7711` (sequential fallback) to
**0.663 ms** with `LionCount`. This is an aggregate-pushdown gain: with pushdown off,
Lion's bitmap range count takes **152.083 ms**, versus B-tree's **1.629 ms** index-only
scan. Do not generalize the count result to fetching the matching rows. Small changes in
other cases are not established improvements from three timing rounds.

## Dirty data: the visibility-map trade-off

After scattered payload updates to 1% of rows and ANALYZE, without VACUUM, Lion must check
visibility in the heap for affected pages. That 1% is a row fraction, not a heap-page fraction;
the [raw operations](../bench/results/2026-09-24-118f623-pg18-focused/operations.jsonl) record actual all-visible coverage.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms | Speedup vs B-tree / GIN |
| --- | --- | --- | --- | --- | --- |
| 5M scalar | Count ~50% / dirty | 711.865 | 829.709 | 81.315 | 8.8× / 10.2× |
| 5M scalar | Count ~0.5% / dirty | 5.940 | 61.288 | 5.608 | 1.1× / 10.9× |
| 5M scalar | Two equality predicates / dirty | 18.284 | 171.558 | 3.962 | 4.6× / 43.3× |
| 5M scalar | Count per 200 groups / dirty | 925.271 † | 962.271 † | 283.387 | 3.3× / 3.4× |

Vacuum cadence and update distribution matter: do not apply the clean-data speedups to a frequently
updated table. The dirty 0.5%-selectivity count is effectively at parity with B-tree here
(5.608 versus 5.940 ms), despite its large clean-data advantage. The suite also checks results
after indexed writes, deletes, and VACUUM.

**Version caveat — earlier PG16–20 runs (`eb1579e`):** ordinary reads on PG19/20 can restore
all-visible pages during heap pruning. In that scattered-update fixture, reference scans and
warmups restored almost all visibility before timing on **19beta4 and 20devel**.
The 5M-row dense count then took about **2 ms**, versus
**74–80 ms on PG16–18**. These are warmed post-update reads, not equivalent dirty-page conditions
or a general 40× Lion speedup; the first read's cleanup cost is outside the timed samples.
Clean dense counts in that earlier comparison were about **1.8–2.1 ms** across all five versions.
See the [per-sample visibility evidence from those earlier runs](../bench/results/2026-09-24-118f623-pg18-focused/earlier-version-visibility.csv).
The headline tables above remain PostgreSQL 18 measurements.

## Where it is at practical parity: tiny probes and fetching rows

Selective equality counts finish in tens of microseconds for both B-tree and Lion. The absolute
gap is too small to justify an additional index on these three samples alone. Fetching payloads
requires heap access and does not benefit from count pushdown.

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms |
| --- | --- | --- | --- | --- |
| 1M scalar | Count ~0.005% | 0.020 | 0.056 | 0.013 |
| 5M scalar | Count ~0.005% | 0.034 | 0.975 | 0.017 |
| 5M scalar | Rare equality (up to 1M keys) | 0.035 | 0.382 | 0.019 |
| 1M scalar | Sum ID/payload length for `c200=17` | 3.445 | 3.482 | 3.053 |
| 5M scalar | Sum ID/payload length for `c200=17` | 14.828 | 57.216 | 16.280 |
| 200k documents | Sum ID/payload length for full-text `w1` | — | 4.179 | 4.325 |

“Practical parity” means similar scale or no demonstrated useful advantage, not statistical
equivalence. Heap-fetch results are cache-sensitive, especially at 5M rows; differences between
Lion and pushdown-off executions of the same bitmap plan are not a count-pushdown benefit.

## Where Lion loses: ordering, phrase and prefix search

| Dataset | Query / state | B-tree ms | GIN ms | Lion ms |
| --- | --- | --- | --- | --- |
| 5M scalar | Ordered LIMIT 100 | 0.021 | 720.948 † | 857.024 † |
| 200k documents | Phrase `common <-> w1` | — | 6.997 | 32.481 † |
| 200k documents | Prefix `rare12:*` | — | 1.552 | 25.077 † |

Lion does not provide ordered row retrieval and stores no full-text positions or sorted lexemes
for phrase/prefix pruning. These queries use sequential fallbacks here. Keep B-tree for ordered
retrieval and GIN for richer full-text search. The report additionally includes skew,
OR/IN intersections, selective three-predicate AND, NULL counts, and low-memory cases.
FK-side join pushdown is not timed by this benchmark.
[Exact SQL and workload manifest](../bench/results/2026-09-24-118f623-pg18-focused/queries.json).

## Build time and index storage

One build per index; times are summed across each portfolio and sizes are measured before mutations.
Scalar portfolios contain seven indexes and documents two. Pushdown changes neither construction
nor storage. These are portfolio totals: high-cardinality columns can change the space trade-off,
and Lion is not smaller for every distribution.

At 5M rows, Lion uses about **25% less space than B-tree** and **23% more than GIN**;
at 1M, it uses about **15% more than either**. Its scalar build takes roughly **2.8–2.9×**
the B-tree time. The additional high-cardinality and skew indexes matter to these totals.

These runs predate NARROW containers (DESIGN.md §38). A dense container is now a NARROW bitmap
only as wide as its heap pages' offsets - 520 bytes for pages of at most 63 rows, 1032 for 127, and
so on to 2568 for the 291 an 8 kB page can hold - where it was a 4104-byte bitset. On synthetic
tables of 1M rows, a two-valued column went from 15.34, 11.40 and 9.31 bits a row to 3.28, 2.49
and 3.41 at 136, 185 and 226 rows a page, and its counts and GROUP BYs became about twice as fast;
at 65 rows a page, 2M rows went from 30.93 to 4.65 bits a row (GIN: 8.91), with counts and GROUP
BYs 2.8 to 4 times faster. Columns whose containers are arrays or runs are unchanged.

| Dataset | Family | Build seconds | Index MiB |
| --- | --- | --- | --- |
| 1M scalar | B-tree | 1.692 | 58.984 |
| 1M scalar | GIN | 2.000 | 58.680 |
| 1M scalar | Lion | 4.656 | 67.797 |
| 5M scalar | B-tree | 8.958 | 256.289 |
| 5M scalar | GIN | 7.622 | 156.961 |
| 5M scalar | Lion | 25.723 | 192.484 |
| 200k documents | GIN | 0.304 | 6.375 |
| 200k documents | Lion | 1.732 | 7.672 |

## Writes and maintenance: 5M rows

Each portfolio inserts 50k rows, updates `c200` in 50k existing rows, deletes rows where
`id % 100 = 1`, then runs VACUUM ANALYZE. A checkpoint precedes each operation. Cells show
**wall time in ms / WAL in MiB**, including all seven indexes. These are single observations;
GIN's fast updates defer work, so include its VACUUM/drain cost.

| Operation | B-tree ms / MiB | GIN ms / MiB | Lion ms / MiB |
| --- | --- | --- | --- |
| INSERT | 983.2 / 119.58 | 344.6 / 61.37 | 1,228.0 / 176.91 |
| Indexed UPDATE | 2,533.0 / 138.53 | 756.4 / 77.89 | 1,675.6 / 209.36 |
| DELETE | 1,871.6 / 366.12 | 1,007.5 / 366.12 | 1,139.8 / 366.12 |
| VACUUM ANALYZE | 3,439.5 / 634.38 | 3,554.4 / 587.33 | 3,750.8 / 569.33 |

This run uses Lion's custom WAL manager; earlier quick measurements used generic WAL and a smaller
portfolio. Do not attribute their difference solely to code changes. The focused suite does not
measure sustained concurrent-write throughput.

Here Lion's insert is **1.25× slower than B-tree**, while its indexed UPDATE takes about
**0.66× the time**; both generate roughly **1.5× the WAL**. GIN's insert/update steps are faster
than Lion's, before its deferred work is drained. Lion's VACUUM is slower than both B-tree
and GIN in this run.

## Reproduce and earlier measurements

Build/install against the desired release PostgreSQL version, then:

```sh
python3 bench/comprehensive/run.py --prefix /path/to/postgresql --preload \
  --output bench/results/my-focused-run
python3 bench/comprehensive/report.py bench/results/my-focused-run
python3 bench/comprehensive/audit.py bench/results/my-focused-run
```

The defaults retain 1M/5M rows and 200k documents. See the [focused suite documentation](../bench/comprehensive/README.md)
for coverage and `--profile full`; [quick.py](../bench/QUICK.md) remains the smaller progress check.
The [previous quick report](../bench/results/quick/2026-09-22-3906538/REPORT.md),
[older comparison](../bench/COMPARISON.md), [growth/churn supplement](../bench/results/2026-09-21-stress/REPORT.md),
and [historical README archive](../bench/HISTORICAL_README_BENCHMARKS.md) describe earlier commits and workloads.
The [follow-up review](../FOLLOWUP_REVIEW.md) records correctness and compatibility validation.
