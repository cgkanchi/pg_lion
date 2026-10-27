# lionvec's 8-bit sum in integers (`lionvec_dot8()`), in a warm scan (2026-10-27)

What the integer kernel of DESIGN.md §44.3 ("Kernels", 2026-10-27; the one §44.14 asked for)
does to a warm query: `lionvec_estimate8()` against the query rounded to 16-bit integers,
`sum(c_i w_i)` and `sum(c_i)` exact in integers (VPMADDWD and VPSADBW under AVX2), in place of
the 16-lane double sum `lionvec_sum8()`.

**Summary.** On 100,000 random unit vectors of 1536 dimensions, cosine, `LIMIT 10` at the
defaults, every page in shared buffers: 45.7 ms a query before and 36.9 after at the sessions'
medians, each query at 0.81 of its old time at the median of 200 before/after pairs (quartiles
0.78 and 0.85). Recall is 0.632 for every query under both libraries, row for row; each query
probes 547.8 partitions and refines 14,957 rows on average under both (4 of the 50 queries'
candidates or refines differ by a few).

## Setup

`scanbench.py` (here), on this machine's PostgreSQL 16.15 (Ubuntu's package, 4 GB of shared
buffers, `work_mem` 64 MB, parallel query off, `jit` off, `fsync` off) with Ubuntu's pgvector
0.6.0, which has no `halfvec`: the AM and the `vector` operator classes were created from the
extension's SQL without the `halfvec` classes. A 4-vCPU VM of an Intel Xeon at 2.1 GHz with
AVX-512 and VPOPCNTDQ (the AVX-512 set, which runs the AVX2 kernel for these sums), gcc 13.3.

`scanbench.py setup 100000 1536`: `items(id int, embedding vector(1536))`, 100,000 unit vectors
of Gaussian coordinates (`numpy`, seed 12345), in TOAST as pgvector stores them, loaded by
binary COPY in 9 s; `CREATE INDEX ... USING lionvec (embedding vector_cosine_ops)` by the old
library in 104 s (219 MB; the heap 5 MB and its TOAST relation about 620 MB); 50 more unit
vectors as the queries, their exact ten nearest by `numpy`. The index is built once and read by
both libraries: the format does not change, only the kernel.

`scanbench.py run LABEL 5`: a new session, `pg_prewarm()` of the heap, its TOAST relation and
the index, `client_min_messages = debug1` for the scan's counts, every query once to warm, then
5 rounds of the 50 queries, each timed around `execute` and `fetchall` from `psycopg`; the
median of a query's 5 times is its time, and the run's figure is the median of those. Recall is
the ten returned against the exact ten. The two libraries (`pg_lion.so` built from the base
commit and from this one, the file replaced by `mv` between sessions) alternated over 4
sessions each, `before-q1`, `after-q1`, ..., `after-q4`, on an otherwise idle machine; the
`canary_ms` in each JSON is a fixed `numpy` matmul's time, a check that the machine was idle
(it is 3.6 to 9.6 ms in these runs, 25 to 60 under load).

## Results

`run-{before,after}-q{1..4}-*.json`: `median_ms` a query, `recall`, `work` (partitions probed,
passes, candidates, refined, fetched, returned) a query, and the run's summaries.

| session | before, ms | after, ms |
| --- | --- | --- |
| 1 | 45.79 | 39.23 |
| 2 | 46.97 | 36.39 |
| 3 | 45.61 | 35.72 |
| 4 | 42.80 | 37.34 |
| median | 45.70 | 36.86 |

The queries' own times run from 24.5 to 70.4 ms before. Over the 200 (query, session) pairs
the new library's time over the old one's is 0.814 at the median, 0.780 and 0.848 at the
quartiles, 0.659 at the least and 0.983 at the most.

Every query's work is the same in every session of a library. Between the libraries 4 queries
differ: 747,881 rows refined in all against 747,865, and 27,389 partitions probed against
27,388 - the bounds moved by the rounding's variance, which the estimate's error now carries.
Recall is identical, query for query.

## The kernels alone

`costs.txt`: `./test/unit/vecplanes_eval --costs` of this tree, pinned to a core (§44.14's
harness, built at `-O1` as the unit tests are): ns a refine at 384 and 1536 dimensions, warm,
from a 24 MB pool and with the lines flushed, for `lionvec_sum8()` (the old sum, kept as the
unit test's reference), the prototype's `idot16` ("bytes, 16-bit query") and `lionvec_dot8()`,
under every implementation. `costs-before.txt` is the base commit's run, without the new row.
