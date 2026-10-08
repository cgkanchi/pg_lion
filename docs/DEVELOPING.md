# Developing pg_lion

Building, testing and finding your way around the source. Section numbers (§) refer to [DESIGN.md](../DESIGN.md).

## Supported versions and CI

Status: prototype.  Builds against PostgreSQL 16, 17, 18, 19 and master (20devel); the version
differences live in `src/lion_compat.h`, and the first validation across all of them (16.15, 17.11,
18.6, 19beta4 and master) was at `eb1579e`.  CI (`.github/workflows/ci.yml`) builds and tests every
one of those majors on each push: the container and sparse unit tests, every SQL regression file in
`test/sql` and every isolation spec in `test/isolation` in both WAL modes, and, on the 19 and master
source builds, the crash-recovery/hot-standby harness in both modes.  One gap is deliberate and worth
knowing: the isolation specs that park a backend on an injection point (`grep -l injection_points
test/isolation/*.spec`), which are the race tests of the VACUUM/count/split interlocks, need a server
configured with `--enable-injection-points` and the `injection_points` test module.  PostgreSQL 16
has no injection points, and the PGDG packages of 17 and 18 ship without the module, so CI runs those
specs only on its 19 and master source builds; on 16-18 the interlocks are covered by the same code
and by those specs on the newer majors, not by a test on that major.  See the
[latest review](../FOLLOWUP_REVIEW.md) for evidence and remaining limitations.

## Build and test

    ./dev.sh reset                                   # initdb a private cluster in .local/data (needs .local/pg)
    make PG_CONFIG=.local/pg/bin/pg_config && make PG_CONFIG=.local/pg/bin/pg_config install
    eval "$(./dev.sh env)"
    make PG_CONFIG=.local/pg/bin/pg_config installcheck   # regress files + isolation specs
    make unit PG_CONFIG=.local/pg/bin/pg_config           # container and sparse libraries, no server needed

No `-march` is needed for speed: on x86-64, GCC and clang builds carry AVX2 and POPCNT versions of
the container library's passes over whole bitsets, compiled with target attributes, and pick one at
run time by what the CPU has; other targets and compilers get a portable one, and `make
LION_NO_SIMD=1` leaves the x86-64 ones out (DESIGN.md §3, "Whole-bitset kernels").  `make unit` runs
the container tests against each version the CPU has, and once more built without the x86-64 ones
(`test/unit/container_test_nosimd`).

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

- **contrib: `citext`, `pageinspect`, `pg_buffercache` and `pg_walinspect`.**  The regression
  suite creates all four.  Without `pageinspect` the `build`, `corrupt`, `corrupt_items` and
  `summary` files fail, without `pg_buffercache` the `corrupt`, `corrupt_items`, `pinbudget` and
  `range` files and the `count_batch_race` and `gettuple_pause` specs fail, without
  `pg_walinspect` `walrecords` fails, and most files use `citext`.  The PGDG packages
  (`postgresql-N`) include contrib; a source build needs `make -C contrib install`.
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
  needs `RECOVERY_RUN_AS=<unprivileged user>` (`test/recovery/README.md`).  Its phases 1b-1e and 3
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

## Source layout

    src/lion_tid.h          TID <-> (container key, 15-bit lo) encoding; 9 offset bits at 8K pages
    src/lion_container.[ch] container library (array/bitset/run/narrow), set algebra, AVX2/POPCNT bitset kernels
                            picked at run time, unit-tested standalone
    src/lion_sparse.[ch]    sparse (container key, offset) segments, unit-tested standalone
    src/lion.h              on-disk structs, the relation state, and the page layer's and the AM's functions
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
    src/lion_insert.c       aminsert (serialised on the directory leaf; bitset in-place fast path)
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
    src/lion_customscan.h   the LionCount custom scan's private header, shared by its planner half:
    src/lion_plan_*.c       create_upper_paths_hook -> CustomPath/CustomScan "LionCount"; set_join_pathlist_hook
                            -> "LionSemiJoin"/"LionAntiJoin"; index matching, partitions, the cost model
    src/lion_exec_*.c       and its executor half: begin, locating the WHERE sets, counting, the FK-side
                            join, the run and parallel DSM, EXPLAIN
    src/lion_costs.[ch]     the cost model's constants as planner settings (pg_lion.*_cost)
    src/lion_fkjoin.[ch]    the FK-side joins a LionCount answers (fact JOIN dim, EXISTS / NOT EXISTS), and the semi/anti join paths
    src/lion_ordered.c      CustomScans "LionOrdered" and "LionBtreeScan": lion-filtered walks of a btree (its
                            values from the index tuples when it covers the query) or of a lion column's order
    src/lion_multikey.c     array_ops/tsvector_ops: GIN-style extraction and tsquery key trees
    test/sql, test/isolation, test/unit, test/recovery, test/modules
