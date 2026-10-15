# The DBpedia benchmark on a workstation

`knn_dbpedia.py` (lionvec, pgvector HNSW, VectorChord and pgvectorscale's diskann, warm and cold,
unfiltered and at 10%, 3% and 1% filters) run end to end on one machine, at the full million rows
of `Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-1M` by default rather than the
115k of `bench/results/2026-10-15-dbpedia-pg18/`. Written for Ubuntu under WSL2 on a 32 GB
desktop; any Ubuntu or Debian works.

```sh
git clone https://github.com/cgkanchi/pg_lion ~/pg_lion      # on the Linux side, not /mnt/c
cd ~/pg_lion
bench/vector/local/setup.sh      # once: packages, pg_lion, venv, the cluster (asks for sudo)
bench/vector/local/run.sh        # everything; resumes where it stopped if interrupted
```

Results land in `bench/results/<date>-dbpedia-local-1000k/`: one JSON file per index (warm),
`cold.json`, `tables.md` (recall against server time, and the cold table), `machine.txt` (CPU,
RAM, disk, PostgreSQL settings) and `sizes.txt`. Logs are in `~/lionbench/logs/`.

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
  45 GB at 1M rows: 9.5 GB of shards, 6 GB of the shards as a numpy array, 8 GB of table (each
  1536-d vector takes its own 8 KB TOAST page), indexes of about 2 GB (lionvec), 4 GB (HNSW),
  8 GB (VectorChord) and 0.7 GB (diskann), and WAL. WSL's virtual disk grows to fit and does not
  shrink afterwards.
- **Cold runs.** `setup.sh` installs `/usr/local/sbin/lionbench-drop-caches` and a sudo rule for
  it alone, so the cold step can empty Linux's page cache between queries without a password.
  Whether Windows also caches WSL's virtual disk underneath is not verified: if the cold times
  come out close to the warm ones, that is the likely cause.

## What it does

`setup.sh` installs PostgreSQL 18, its headers and pgvector from apt.postgresql.org, VectorChord
1.1.1's `.deb` from its GitHub release, and pgvectorscale from Timescale's apt repository (if that
fails, diskann is skipped; `SKIP_DISKANN=1` skips it outright). It builds and installs pg_lion from
this checkout, makes a venv in `~/lionbench/venv`, and creates the benchmark's own cluster in
`~/lionbench/pgdata` (port 55440, socket in `~/lionbench/sock`; it does not touch `dev.sh`'s
cluster or any system cluster). The cluster takes the project's standing settings
(`random_page_cost` 1.1, `effective_io_concurrency` and `maintenance_io_concurrency` 200,
`effective_cache_size` 10 GB, `io_method` worker, no parallel query or build, `jit` and `fsync`
off), with `shared_buffers` a quarter of RAM and `maintenance_work_mem` 35%, each at most 8 GB
(HNSW's build needs its 3.5 GB graph in `maintenance_work_mem` to stay fast). `pg.sh` starts,
stops and opens psql on it.

`run.sh` runs these steps, each once (a marker in `~/lionbench/state`; delete it to redo one):

| Step | What |
| --- | --- |
| `fetch` | the first `NSHARDS` shards (default all 26) from Hugging Face, resumable |
| `prepare` | 200 held-out queries and the exact top ten under each filter (float64, chunked) |
| `load` | the table, binary COPY, and the lion index on the filter column |
| `lionvec`, `hnsw`, `vchord`, `diskann` | the builds: lionvec with the build's own partition count, HNSW m 16 / ef_construction 64 on halfvec, VectorChord with sqrt(n) lists (1,000), diskann at its defaults |
| `warm-<kind>` | each index's sweep, every filter, warm |
| `cold` | restart and drop the cache before each query, 7 queries a method, at the settings the warm runs found reach 0.95 and 0.98 |
| `table` | `tables.md` |

`STEPS="warm-lionvec table" run.sh` runs a subset. The sweeps go wider than at 115k
(`KNN_MARGINS` 2 to 32, `KNN_EF` to 800, `KNN_PROBES` to 640); every `KNN_*` variable of
`knn_dbpedia.py` can be set. The warm runs prewarm the index into `shared_buffers` and the table
and its TOAST into the kernel's cache (`KNN_PREWARM=read`), since 8 GB of TOAST does not fit
`shared_buffers`, then run every query once untimed before timing it.

**Smaller runs.** `NSHARDS=8 run.sh` takes 8 shards (about 308k rows) and finishes in a fraction
of the time; `NSHARDS=3` reproduces the 115k set of the earlier results. Use a separate
`BENCH_DIR` for each size, or clear `~/lionbench/state` and drop the database
(`pg.sh psql -c 'DROP DATABASE dbpedia'`) and `~/lionbench/cache` between sizes.

**How long.** Not measured at 1M rows. Scaled from the 115k builds on a 4-vCPU Xeon (lionvec
161 s, HNSW 186 s, VectorChord 22 s, diskann 338 s, all single-process), expect roughly 40 to 70
minutes for lionvec, 20 to 40 for HNSW, 10 for VectorChord and about an hour for diskann, plus an
hour or two of warm sweeps and half an hour cold: an evening or overnight in all.
