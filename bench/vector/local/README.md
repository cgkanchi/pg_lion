# The DBpedia benchmark on a workstation

`knn_dbpedia.py` run end to end on one machine: lionvec against VectorChord's `vchordrq` with its
prefilter, warm and cold, unfiltered and at 10%, 3% and 1% filters, at the full million rows of
`Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-1M` rather than the 115k of
`bench/results/2026-10-15-dbpedia-pg18/`. pgvector's HNSW and pgvectorscale's diskann are left out
by default (`KINDS="lionvec vchord hnsw diskann"` puts them back). Written for Ubuntu under WSL2 on
a 32 GB desktop; any Ubuntu or Debian works.

```sh
git clone https://github.com/cgkanchi/pg_lion ~/pg_lion      # on the Linux side, not /mnt/c
cd ~/pg_lion
bench/vector/local/setup.sh      # once: packages, pg_lion, venv, the cluster (asks for sudo)
bench/vector/local/run.sh        # everything; resumes where it stopped if interrupted
```

`setup.sh` uses the PostgreSQL 18 already installed: apt.postgresql.org's
(`/usr/lib/postgresql/18`) or the one whose `pg_config` is on `PATH`. For a source build elsewhere,
`PG_CONFIG=/path/to/bin/pg_config bench/vector/local/setup.sh` (and the same `PG_CONFIG` for
`run.sh`). It adds pgvector and VectorChord to it and leaves its version and its own cluster alone.
Only with `INSTALL_PG=1` does it install PostgreSQL itself. Stopping your own cluster during the
run frees its memory (`sudo service postgresql stop`, or `pg_ctl stop` for a source build).

Results land in `bench/results/<date>-dbpedia-local-1000k/`: `lionvec.json` and
`vchord-l1000.json` (warm), `cold.json`, `tables.md` (recall against server time, and the cold
table), `machine.txt` (CPU, RAM, disk, PostgreSQL settings) and `sizes.txt`. Logs are in
`~/lionbench/logs/`.

## Before the first run, under WSL

- **Memory.** WSL2 gives Linux half the PC's RAM by default (16 GB of 32). `setup.sh` prints what
  Linux sees. For the million rows, raise it: in `%UserProfile%\.wslconfig` on Windows put

  ```ini
  [wsl2]
  memory=24GB
  swap=8GB
  ```

  then run `wsl --shutdown` from Windows and reopen the shell. 16 GB still works (the cluster sizes
  itself to what it sees), but less of the warm working set stays in memory.
- **Disk.** Keep the checkout and `BENCH_DIR` (default `~/lionbench`) on the Linux filesystem;
  `/mnt/c` is 9p and an order of magnitude slower, and `setup.sh` refuses it. The run needs about
  40 GB at 1M rows: 9.5 GB of shards, 6 GB of the shards as a numpy array, 8 GB of table (each
  1536-d vector takes its own 8 KB TOAST page), indexes of about 2 GB (lionvec) and 8 GB
  (VectorChord keeps every vector for its rerank), and WAL. WSL's virtual disk grows to fit and
  does not shrink afterwards.
- **Cold runs.** `setup.sh` installs `/usr/local/sbin/lionbench-drop-caches` and a sudo rule for
  it alone, so the cold step can empty Linux's page cache between queries without a password.
  Whether Windows also caches WSL's virtual disk underneath is not verified: if the cold times
  come out close to the warm ones, that is the likely cause.

## What it does

`setup.sh` adds pgvector (VectorChord needs its types) and VectorChord 1.1.1 to that PostgreSQL:
for an apt.postgresql.org install, `postgresql-18-pgvector` and VectorChord's release `.deb`; for
any other, pgvector 0.8.1 built from source and the `.deb`'s files copied into its directories.
`WITH_DISKANN=1` also installs pgvectorscale from Timescale's apt repository. It builds and
installs pg_lion from this checkout, makes a venv in `~/lionbench/venv`, and creates the benchmark's own cluster in
`~/lionbench/pgdata` (port 55440, socket in `~/lionbench/sock`; it does not touch `dev.sh`'s
cluster or any system cluster). The cluster takes the project's standing settings
(`random_page_cost` 1.1, `effective_io_concurrency` and `maintenance_io_concurrency` 200,
`effective_cache_size` 10 GB, `io_method` worker, no parallel query or build, `jit` and `fsync`
off), with `shared_buffers` a quarter of RAM and `maintenance_work_mem` 35%, each at most 8 GB
(enough for HNSW's 3.5 GB graph, if it is added). `pg.sh` starts,
stops and opens psql on it.

`run.sh` runs these steps, each once (a marker in `~/lionbench/state`; delete it to redo one):

| Step | What |
| --- | --- |
| `fetch` | the first `NSHARDS` shards (default all 26) from Hugging Face, resumable |
| `prepare` | 200 held-out queries and the exact top ten under each filter (float64, chunked) |
| `load` | the table, binary COPY, and the lion index on the filter column |
| `build` | lionvec with the build's own partition count (8 sqrt(n), spill on); VectorChord with sqrt(n) lists (1,000), spherical centroids, no residual quantization, as at 115k |
| `warm` | each index's sweep under every filter, warm: lionvec margins 2 to 32, through the planner's plan and LionOrdered; VectorChord probes 10 to 640, with and without `vchordrq.prefilter` |
| `cold` | restart and drop the cache before each query, 7 queries a method, at the settings the warm runs found reach 0.95 and 0.98 (unfiltered, and at 3% for LionOrdered and the prefilter) |
| `table` | `tables.md` |

`STEPS="warm cold table" run.sh` runs a subset. At 115k VectorChord did better with fewer lists
(339 against 1,357); to try another count at 1M, `LISTS=4000 STEPS="build warm cold table"
run.sh` afterwards rebuilds its index and writes `vchord-l4000.json` beside the first (the cold
step then measures the new one; move `cold.json` aside first to keep both). Every `KNN_*`
variable of `knn_dbpedia.py` can be set. The warm runs prewarm the index into `shared_buffers` and the table
and its TOAST into the kernel's cache (`KNN_PREWARM=read`), since 8 GB of TOAST does not fit
`shared_buffers`, then run every query once untimed before timing it.

**Smaller runs.** `NSHARDS=8 run.sh` takes 8 shards (about 308k rows) and finishes in a fraction
of the time; `NSHARDS=3` reproduces the 115k set of the earlier results. Use a separate
`BENCH_DIR` for each size, or clear `~/lionbench/state` and drop the database
(`pg.sh psql -c 'DROP DATABASE dbpedia'`) and `~/lionbench/cache` between sizes.

**How long.** Not measured at 1M rows. Scaled from the 115k builds on a 4-vCPU Xeon (lionvec
161 s, VectorChord 22 s, both single-process), expect roughly 40 to 70 minutes for lionvec's
build and 10 for VectorChord's, plus about an hour of warm sweeps and 20 minutes cold: two to
three hours in all, after the 9.5 GB download.
