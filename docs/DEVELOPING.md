# Developing pg_lion

Building, testing and finding your way around the source. Section numbers (§) refer to [DESIGN.md](../DESIGN.md).

## Supported versions and CI

Builds against PostgreSQL 16, 17, 18, 19 and master (20devel); the version differences live in
`src/lion_compat.h`.  It needs a POSIX `mmap()`: LionOrdered's set, past `hash_mem`, lives in a
temporary file mapped into memory (`src/lion_arena.c`, DESIGN.md §30.4, "Spilling").  CI (`.github/workflows/ci.yml`) builds and tests, on
each pull request, 16 to 19 as the PGDG packages and 18 built from source
with assertions and injection points, and on every one of them
runs the unit tests (as built, and under ASan and UBSan), the SQL regression files in `test/sql`
and the isolation specs in `test/isolation` in both WAL modes, `make hookcheck`, and the
crash-recovery/hot-standby harness (`test/recovery/run.sh`) in both WAL modes with
`wal_consistency_checking`.  The other source builds - 16, 17, 19 and master, with assertions,
and from 17 with injection points - run when `.github/workflows/optional.yml` is started by hand
from the Actions tab, which builds all five by default; master is tested only there.  The same
workflow runs 18's packages on arm64, which is not a first-class target.  On the source builds pg_lion itself is compiled with UBSan
(`COPT=-fsanitize=undefined -fno-sanitize-recover=all`), so the whole suite runs under it and a
report aborts the backend that met it.  What parks a backend on an injection point - the isolation
specs that do (`grep -l injection_points test/isolation/*.spec`), which are the race tests of the
VACUUM/count/split interlocks, and the harness's phases 1b-1e, 1g and 3 - needs a server configured
with `--enable-injection-points` and the `injection_points` test module.  PostgreSQL 16 has no
injection points, and the PGDG packages ship without the module, so CI runs those specs and phases
on its 18 source build (and the 17, 19 and master ones of a run by hand), where a missing module
fails the job
(`INJECTION_POINTS=1`); on 16 and the packages the Makefile leaves the specs out and says so, and
the harness runs its phases 1, 1f, 2, 4 and 5.  The concurrent soak (`test/soak`) runs weekly on
18's packages, and by hand from the Actions tab (`.github/workflows/soak.yml`).
lionvec's tests need pgvector, which every job installs or builds; a failed install or build fails
the job, and `PGVECTOR=1` keeps the Makefile and the harness from skipping them, except on 19's
packages while apt.postgresql.org has none for a beta.  The source builds are also the only ones
with assertions, so `debug_discard_caches` runs there (`discard_caches.sql`, on 16, 19 and master, in a run by
hand), and `make hookcheck` on 16 is where the refusal to read a lion index under `old_snapshot_threshold` is tested (16 alone
has the setting, DESIGN.md §9).

## Build and test

    ./dev.sh reset                                   # initdb a private cluster in .local/data (needs .local/pg)
    make PG_CONFIG=.local/pg/bin/pg_config && make PG_CONFIG=.local/pg/bin/pg_config install
    eval "$(./dev.sh env)"
    make PG_CONFIG=.local/pg/bin/pg_config installcheck   # regress files + isolation specs
    make unit PG_CONFIG=.local/pg/bin/pg_config           # bit-planes, containers, segments, positions,
                                                          # entry layout, lionvec's arithmetic, the read
                                                          # stream ring; no server
    make planes-bench PG_CONFIG=.local/pg/bin/pg_config   # the bit-plane kernels timed, at -O2
    make vecplanes-eval PG_CONFIG=.local/pg/bin/pg_config # lionvec's codes as bit-planes 2-8, evaluated
                                                          # and not adopted (DESIGN.md §44.14); minutes

No `-march` is needed for speed: on x86-64, GCC and clang builds carry AVX2 and POPCNT versions of
the container library's passes over whole bitsets, AVX-512 (VPOPCNTDQ), AVX2 and POPCNT ones of the
bit-plane kernels - lionvec's plane 1 among them - and AVX2 (and F16C) ones of lionvec's others,
compiled with target attributes, and pick one at run time by what the CPU has - one choice for the
extension, `lion_simd_current()` in `src/lion_planes.h`, which every one of them follows; other
targets and compilers get a portable one, and `make LION_NO_SIMD=1` leaves the x86-64 ones out
(DESIGN.md §3, "Whole-bitset kernels"; §44.3, "Kernels"; §50).  `make unit` runs the bit-plane,
container and lionvec tests against each version the CPU has, and once more built without the
x86-64 ones (`test/unit/planes_test_nosimd`, `test/unit/container_test_nosimd`,
`test/unit/vec_test_nosimd`).

Lion logs through one of two WAL resource managers (DESIGN.md §25), chosen per index at
CREATE INDEX, so the suite has to pass in both:

    make PG_CONFIG=.local/pg/bin/pg_config installcheck           # generic WAL (no preload)
    make PG_CONFIG=.local/pg/bin/pg_config installcheck-rmgr      # the custom resource manager
    make PG_CONFIG=.local/pg/bin/pg_config recovery-check       RECOVERY_PREFIX=<prefix>
    make PG_CONFIG=.local/pg/bin/pg_config recovery-check-rmgr  RECOVERY_PREFIX=<prefix>

`installcheck-rmgr` restarts the dev cluster with `shared_preload_libraries = 'pg_lion'` on
pg_ctl's command line, proves the restart took - the server lists pg_lion as a WAL resource manager
and a freshly built index reports `rmgr` - and only then runs the same suite (`walrecords` has an
expected file for each mode, so the suite alone would pass in either), and afterwards puts the
cluster back as it found it: restarted in generic mode, or stopped.  Nothing is written into
postgresql.conf, so an interrupted run leaves no trace; `make hookcheck` works the same way.
`recovery-check-rmgr` is the recovery
harness with the resource manager registered AND `wal_consistency_checking = 'pg_lion'`, which is
where "replay reproduces every page" is actually proved - the comparison only happens during
replay, so turning it on for a primary that never replays proves nothing.

`.local/pg` can be any PostgreSQL 16 or later install.  What the suites need from it:

- **contrib: `citext`, `pageinspect`, `pg_buffercache`, `pg_freespacemap` and
  `pg_walinspect`.**  The regression suite creates all five.  Without `pageinspect` the
  `build`, `corrupt`, `corrupt_items`, `corrupt_records` and `summary` files
  fail, without `pg_buffercache` the `corrupt`, `corrupt_items`, `pinbudget` and `range` files
  and the `count_batch_race` and `gettuple_pause` specs fail, without `pg_freespacemap`
  `dir_delete` fails, without
  `pg_walinspect` `walrecords` fails, and most files use `citext`.  `installcheck` checks for
  all five before it starts and stops with one message if any is missing.  The PGDG packages
  (`postgresql-N`) include contrib; a source build needs `make -C contrib install`.
- **pgvector** 0.7 or later (for `halfvec`) for `vector.sql`, `vector_filter.sql`,
  `vector_half.sql` and the `lionvec_*` isolation specs, lionvec's files (DESIGN.md §44): the PGDG
  package `postgresql-N-pgvector`, or a build of github.com/pgvector/pgvector against the same
  `pg_config`.  Where it is missing those files are skipped and the run says so (`PGVECTOR=1` forces them; `PGVECTOR=0` skips them),
  as is the recovery harness's phase 5, which under `PGVECTOR=1` fails instead.
  pg_lion itself builds and loads without it.
- **Session settings** the expected plans assume (`work_mem`, `maintenance_work_mem`, no parallel
  workers, no JIT) are passed to every connection the suite makes through `PGOPTIONS`
  (`LION_TEST_OPTIONS` in the Makefile), so a cluster other than `./dev.sh`'s gives the same plans.
- **`pg_isolation_regress`** for the isolation specs: a source build installs it with
  `make -C src/test/isolation install`, and the packages ship it in `postgresql-server-dev-N`.
- **`injection_points`** for the specs that park a backend on an injection point: a server
  configured with `--enable-injection-points` (17 or later) and the test module installed with
  `make -C src/test/modules/injection_points install`.  Where the module is missing those specs are
  skipped and the run says which (`INJECTION_POINTS=1` forces them, and makes a missing module an
  error instead).
- **The recovery harness** (`make recovery-check`) needs a whole installation to initdb clusters
  in, named by `RECOVERY_PREFIX`, which it builds and installs the extension into
  (`RECOVERY_SKIP_INSTALL=1` uses the one already installed there instead); as root it also
  needs `RECOVERY_RUN_AS=<unprivileged user>` (`test/recovery/README.md`).  Its phases 1b-1e, 1g and 3
  need `injection_points`, and are skipped without it unless `INJECTION_POINTS=1` makes that an
  error.

The regression files VACUUM with `FREEZE` wherever what they print depends on the VACUUM having
done all its work - index statistics after a DELETE, a plan the visibility map prices, a count
that must skip every heap block.  A plain VACUUM that cannot get a heap page's cleanup lock at
once (the checkpointer and the background writer pin the pages they write) checks that page
without pruning it: its dead TIDs stay in every index and its visibility-map bit stays clear.  An
aggressive VACUUM waits for the lock instead.  The isolation specs keep plain VACUUMs, whose waits
are part of what they test.

`make soak` (`test/soak/README.md`) is the long-running check the suites are not: four pgbench
writers (inserts, updates, deletes, rollbacks, savepoints, upserts, and a phase of writes that all
roll back), a VACUUM loop, and readers that compare every lion-answered query - counts, GROUP BYs,
`count(DISTINCT)`, the aggregates over keys, the direct SQL counts, LionOrdered, bitmap and plain
scans - with a sequential scan inside the same REPEATABLE READ or SERIALIZABLE snapshot, some of
them holding it across VACUUMs, plus periodic `lion_index_verify(idx, true)`; ten minutes in each
WAL mode, with a hot standby whose readers compare the same way in rmgr mode.  It restarts the dev
cluster as `installcheck-rmgr` does and puts it back; `test/soak/soak.sh` runs it against any
running cluster.  Races it found are kept as reproducers in `test/soak/findings/`, whose expected
outputs hold the correct answers (`test/soak/findings/run-spec.sh`).

`dev.sh` puts its cluster's socket in `$XDG_RUNTIME_DIR/pg_lion-<user>` (or `/tmp/pg_lion-<user>`)
on port 54329, and `LION_SOCK` / `LION_PORT` move it.  The cluster trusts local connections, so
`dev.sh` makes a missing socket directory mode 0700, and refuses the default one if it is a symlink,
belongs to someone else or is open to others.  The cluster runs with `wal_level = replica`, so
that index builds and the write paths of indexes created in the same transaction are WAL-logged in
the suite as they are in production.

## Changing the SQL scripts

An installed extension's functions and operator classes change only through `ALTER EXTENSION
... UPDATE`, so a change to `pg_lion--<version>.sql` (or pg_lion_citext's, or pg_lion_vector's)
is a new version (§23, "Extension versions"):

1. `git mv pg_lion--0.1.0-devN.sql pg_lion--0.1.0-devN+1.sql`, make the change there, and set
   `default_version` in `pg_lion.control` and `provides.pg_lion.file` in META.json to match.
2. Add `pg_lion--0.1.0-devN--0.1.0-devN+1.sql` making the same change to an installed copy:
   `CREATE OR REPLACE FUNCTION` where the arguments and result stay the same, `DROP FUNCTION`
   then `CREATE FUNCTION` where they or the OUT columns change, `ALTER OPERATOR FAMILY ... ADD`
   for new strategies and support functions, `CREATE OPERATOR CLASS` for new classes, and the
   same `COMMENT`, `GRANT` and `REVOKE`.  Never edit an upgrade script that is on main.
3. Run the checks, which CI runs too (`.github/workflows/upgrade.yml`, on 18's packages):

       ./test/upgrade/lint.py origin/main     # versions, script names, META.json, no edits to old scripts
       make upgradecheck                      # every earlier version updated, against a fresh install

   `make upgradecheck` needs the build installed, the dev cluster running, write access to the
   installation's extension directory (it puts each old install script there during its test)
   and the full git history.

A release turns `0.1.0-devN` into `0.1.0` the same way, with an upgrade script that changes
nothing.

## Calibrating the cost model

Two scripts in `bench/calib/`, both Python's standard library and `psql`, best run on a release
build with nothing else on the machine:

    python3 bench/calib/ops.py --setup && python3 bench/calib/ops.py   # lion's operations against core's
    python3 bench/calib/matrix.py --setup-only && python3 bench/calib/matrix.py --out run.json   # plan choices

`ops.py` times each lion-only operation the planner prices against the core operation it stands in
for, on the same build, and prices it at that operation's own rate - its time over core's price of
it, in `cpu_operator_cost`s - printed beside the setting that charges it (DESIGN.md §49,
"Calibration"): a nested loop's B-tree probe against core's Index Scan, apportioned between
`pg_lion.probe_rescan_cost` and `pg_lion.probe_entry_cost`. It prints a sequential scan's rate
too, the slope of its time over its quals, for comparison. `matrix.py` plans and times a fixed set of queries under
every arm and reports the planner's mispicks (DESIGN.md §39, "The matrix"); `--set
pg_lion.competitor_pricing=off` runs it with lion's paths priced by their own work alone.
`--setup-file bench/calib/joins.sql --queries bench/calib/joins.py` swaps in the joins of DESIGN.md
§40.10 and §46 to §49 at scale (a 4.7 GB fixture at `--rows 4000000`), `--arms` plans under the
named arms alone (`probejoin`, `ordjoin` and `probe` force lion's joins), `--sweep A+B=v+w,...`
sweeps settings together, and `--cold CMD` runs CMD - one that stops the server, drops the
database's files from the OS cache and starts it - before each timed run.

## Source layout

    src/lion_tid.h          TID <-> (container key, 15-bit lo) encoding; 9 offset bits at 8K pages
    src/lion_planes.[ch]    bit-plane arithmetic - popcounts of a bit vector AND each of k planes, interleaved
                            or each its own array, their weighted sum, the 64x64 bit transpose and values to
                            and from planes - and the extension's one choice of SIMD implementation
                            (portable, POPCNT, AVX2, AVX-512); below the container library and lionvec,
                            frontend-safe and unit-tested standalone (test/unit/planes_test.c)
    src/lion_container.[ch] container library (array/bitset/run/narrow), set algebra, AVX2/POPCNT bitset kernels
                            picked by that choice at run time, unit-tested standalone
    src/lion_sparse.[ch]    sparse (container key, offset) segments, unit-tested standalone
    src/lion.h              on-disk structs, the relation state, and the page layer's and the AM's functions
    src/lion_entryfmt.h     the directory entry tuple's byte layout and accessors, unit-tested standalone
    src/lion_pages.c        index pages: initializing, deleting, recycling; the items of container pages
    src/lion_meta.c         the meta page
    src/lion_state.c        per-relation state: columns, opclass procedures and order; usability checks; keys
    src/lion_entry.c        entry tuples, and the max_entries cardinality guard
    src/lion_posting_put.c  writing container chains: putting items, spilling an INLINE entry, page splits
    src/lion_compat.h       the differences between PostgreSQL 16, 17, 18, 19 and master
    src/lion_wal.[ch]       the WAL shim every write path calls, and the custom resource manager
    src/lion_dir.c          the sorted entry directory: a Lehman & Yao B-tree keyed by the index key
    src/lion_posting.c      the per-key posting tree: a B-tree over container keys, GIN's shape
    src/lion_am.c           handler, reloptions, amvalidate, table AMs, buildempty, _PG_init hook/GUC
    src/lion_amcost.c       amcostestimate: the cost of plain and bitmap scans
    src/lion_build.c        ambuild via tuplesort (hash, key, tid code); INLINE entries or per-key posting trees
    src/lion_scan.c         amgetbitmap, and amgettuple for plain index scans
    src/lion_insert.c       aminsert (serialised on the directory leaf; bitset in-place fast path; a row's
                            bit slices a walk of the leaf a column, their in-place adds several to a record)
    src/lion_vacuum.c       ambulkdelete with cleanup locks on every page, two-pass cancellable protocol
    src/lion_funcs.[ch]     lion_index_stats() and the other diagnostics; lion_funcs.h is private to them
    src/lion_verify*.c      lion_index_verify(): pages and posting sets, the directory, rechecks and
                            heapallindexed, summary posting sets
    src/lion_count.h        the count engine's interface - lion_count_keys(): VM-interlocked counting,
                            per-block batched heap recheck - and lion_count_int.h what its files share:
    src/lion_set.c          locating posting sets, the list pin budget, the lookup walk
    src/lion_set_copy.c     private copies of posting sets, spilled to a file when too big
    src/lion_cursor.c       container cursors over one posting set
    src/lion_expr.c         expression cursors over boolean expressions of sets; the stream the scans read
    src/lion_vis.c          the visibility map a container at a time, and the per-query visibility cache
    src/lion_count.c        counting a container against the map and the heap; the merge
    src/lion_count_groups.c the groups of a walk counted together
    src/lion_count_shared.c the copy the participants of a parallel plan share
    src/lion_rangesrc.c     a range as a source: collected, or probed at the other sources' rows
    src/lion_range.c        range restrictions, and iterating every entry of an index
    src/lion_count_sql.c    the SQL count functions (lion_index_count() and the others)
    src/lion_facets.c       lion_index_facets(): the rows under each key of a column, under a filter
    src/lion_slice.c        a bit-sliced column's counts and descents (LionSliceAgg), lion_index_slice_agg()
    src/lion_slice_decode.c a sliced column's aggregates for a batch of groups, decoded key by key
    src/lion_customscan.h   the LionCount custom scan's private header, shared by its planner half:
    src/lion_plan_*.c       create_upper_paths_hook -> CustomPath/CustomScan "LionCount"; set_join_pathlist_hook
                            -> "LionSemiJoin"/"LionAntiJoin"; index matching, partitions, the cost model
    src/lion_exec_*.c       and its executor half: begin, locating the WHERE sets, counting, the FK-side
                            join, the run and parallel DSM, EXPLAIN
    src/lion_costs.[ch]     the cost model's constants as planner settings (pg_lion.*_cost)
    src/lion_fkjoin.[ch]    the FK-side joins a LionCount answers (fact JOIN dim, EXISTS / NOT EXISTS), and the semi/anti join paths
    src/lion_ordered.c      CustomScans "LionOrdered" and "LionBtreeScan": lion-filtered walks of a btree (its
                            values from the index tuples when it covers the query) or of a lion column's order
    src/lion_probejoin.c    CustomScan "LionProbeJoin": a GROUP BY of a join to a dimension by its primary
                            key, each fact row's key looked up in the B-tree and tested against lion sets
    src/lion_ordjoin.c      CustomScans "LionOrderedJoin": a fact's btree walked in order across a join to a
                            dimension's unique key, both sides filtered by lion sets, key-only tables as hash sets;
                            and "LionOrderedSplit": that join run once per entry of a dimension column that leads
                            the ORDER BY, the dimension narrowed to the entry's rows
    src/lion_multikey.c     array_ops/tsvector_ops: GIN-style extraction and tsquery key trees
    src/lion_jsonb.c        jsonb_contains_ops: path keys for jsonb @>, ?, ?|, ?&, @? and @@
    src/lion_vec.[ch]       lionvec, the nearest-neighbour AM of pgvector's vector and halfvec (created by
                            the pg_lion_vector extension): pages, build, inserts, VACUUM, costs, handler;
                            lion_vec.h is what LionOrdered uses to restrict its scan to a lion set
    src/lion_vec_scan.c     its scan: passes over the 1-bit codes, the lazy queue of bounds, refines and
                            exact distances
    src/lion_vec_loc.c      its locator: where vectors in TOAST lie, so that the scan reads them from their
                            chunks' pages directly - the lookups, the checked reads, LOC and LOCDIR pages
    src/lion_vec_math.[ch]  its rotation, bit-plane codes, kernels and error bounds - plane 1's kernel
                            lion_planes.c's, and every kernel run by lion_planes' choice - frontend-safe
                            and unit-tested standalone (test/unit/vec_test.c)
    src/lion_readahead.[ch] reading a relation's blocks in an order its reader predicts: a read stream
                            over the blocks it pushes, or over a list it knows whole (17 and later), asks
                            of the kernel, or plain reads
    src/lion_readahead_ring.[ch]  the read stream's bookkeeping: blocks pushed, given, held; frontend-safe
                            and unit-tested standalone (test/unit/readahead_test.c)
    test/sql, test/isolation, test/unit, test/recovery, test/modules
