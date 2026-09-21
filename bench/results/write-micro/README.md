# write_micro results (2026-09-21, v1 write wave)

Produced by `bench/write_micro.sh` on its own cluster (fsync on,
full_page_writes on, wal_level replica, checkpoints out of the way), against an
**assert-enabled `-O1 -g` PostgreSQL 20devel** - so every absolute number here
is well above what an optimized build gives (bench/COMPARISON.md's portfolio
build is 6.7 s where this box measures 10.5 s).  WAL byte counts are
deterministic and repeat to the byte; timings on this box drift 10-40% between
sessions, which is why `headline` takes medians over repeats and why the
before/after timing claims come from `ab_*` (the two builds interleaved in one
session, swapping only the installed .so).

| label | what it is |
| --- | --- |
| `before` | git 0e21a97, the whole write wave still to come |
| `after1` | + growth slack and in-place item growth (task 1) |
| `after2` | + the bulk-write build (task 2) |
| `after3` | + the bucket guard, reltuples sizing, folded tail lock (tasks 3, 4) |
| `final` | the wave as it stands, with the warning threshold at four pages |
| `notailfold` | `final` with the folded tail lock taken back out |
| `ab_base_N` / `ab_new_N` | baseline and wave, interleaved, one session |

Files: `LABEL.tsv` (build/insert/burst), `LABEL.headline.tsv` (repeats and
medians), `LABEL.scale.tsv` (tps by clients x key count), `LABEL.waits.txt`
(wait events of the 8-client burst), `LABEL.wal.txt` (WAL composition per
record type, from pg_walinspect).

## The A/B, 2026-09-21 (three interleaved pairs, one session)

| metric | baseline | wave |
| --- | ---: | ---: |
| eight-index build, 1M rows | 13.31 / 12.96 / 14.51 s, 94.62 MiB | 9.93 / 10.46 / 9.66 s, 56.78 MiB |
| insert 10k rows, 8 indexes, after checkpoint | 1148 / 1137 / 1127 ms, 90.94 MiB | 1170 / 1160 / 2050* ms, 85.62 MiB |
| the same again, no checkpoint (steady state) | 1125 / 1131 / 1122 ms, 56.32 MiB | 1120 / 1094 / 1108 ms, 50.71 MiB |

\* that sample overlaps a regression run on the other cluster.

Build: -25% time, -40% WAL.  Single-client inserts: WAL -6% (first batch) and
-10% (steady state), time unchanged - the in-place item growth of task 1 saves
the copy-out/write-back and the item relocation, but what a single-TID insert
spends most of its time on is GenericXLog copying and diffing the two pages of
its record, which is unchanged.  Where task 1 shows up in time is the 8-client
burst's WAL per row (399 -> 262 bytes) and the per-index WAL of a
low-cardinality key (c2: 6.57 -> 2.74 MiB per 10k rows).
