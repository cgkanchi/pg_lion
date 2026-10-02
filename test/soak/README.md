# The soak: concurrent writes, VACUUM and counts, compared row for row

The regression files and the isolation specs each pin one interleaving.  This
directory runs many at once for minutes, against a cluster, and compares every
answer a lion path gives with the answer of a sequential scan **inside the same
snapshot**: concurrent pgbench writers, a VACUUM loop, readers that check, and
periodic `lion_index_verify(idx, heapallindexed => true)`, optionally with a
hot standby whose readers check the same way.  It is what the §9 / §11 / §25
interlock (DESIGN.md) is supposed to survive.

## Running it

Against the dev cluster, in both WAL modes, ten minutes a mode, a hot standby
in rmgr mode:

    make soak                                   # as the cluster's own user
    make soak SOAK_RUN_AS=<user>                # as root: the user the cluster runs as
    SOAK_DURATION=120 SOAK_MODES=rmgr make soak # shorter, one mode

`make soak` runs `test/soak/modes.sh`, which restarts the dev cluster
(`.local/data`) into each mode the way `make installcheck-rmgr` does - the
options on pg_ctl's command line (`-c shared_preload_libraries=pg_lion`, and
`-c max_wal_senders=4` where a standby is wanted), never in postgresql.conf -
proves the mode took (the server's resource managers list pg_lion, or not, and
the soak's indexes report the mode), runs the soak, and puts the cluster back:
running in generic mode if it was running.  Its settings, all optional:

| variable | default | |
|---|---|---|
| `SOAK_MODES` | `generic rmgr` | the modes, in order |
| `SOAK_STANDBY_MODES` | `rmgr` | the modes that also get a hot standby |
| `SOAK_DURATION` | `600` | seconds of load per mode |
| `SOAK_ARGS` | | more `soak.sh` options, e.g. `--rows 100000 --readers 1` |
| `SOAK_RUN_AS` | | the user the cluster runs as, when this runs as root |
| `SOAK_SERVER_OPTS` | | more options for the restart, e.g. `-c shared_buffers=16MB`: a pool small enough that long lists run past the pin budget (DESIGN.md §15) and come out unpinned |
| `SOAK_STANDBY_PORT`, `_SOCK`, `_DATA` | port + 1, `<socket dir>-standby`, `.local/soak-standby` | the standby's endpoint and data directory |
| `LION_SOCK`, `LION_PORT` | | as for `dev.sh` |

`test/soak/soak.sh` is the soak itself, against whatever cluster PGHOST/PGPORT
name, already running in whatever mode (it does not restart anything):

    eval "$(./dev.sh env)"
    test/soak/soak.sh --duration 600 [--rows 300000] [--prows 100000] [--writers 4] [--readers 2]
                      [--expect-mode rmgr] [--server-log .local/pg.log]
                      [--standby-port 54330 --standby-sock DIR --standby-data DIR --run-as USER]
                      [--mixed 75] [--abort 45] [--abort-rate 1] [--verify-every 150] [--seed N]

`soak.sh --help` lists every option.  It creates the database `lion_soak`
(dropping one an earlier run left), so it never touches the regression
databases.  On a sanitized server put `ASAN_OPTIONS` in the environment of the
server and of the soak alike (the client programs are sanitized too); the
server log is scanned for sanitizer reports.

Everything a run writes goes to `test/soak/out/<timestamp>[-<mode>]/`
(ignored by git): `summary.txt`, `failures.log` (each FAIL with its detail and
the server log's tail at that moment), the readers' raw results
(`reader-*.out`, one line per check), `writers.log` (every pgbench phase's
report), `vacuum.log`, `verify-*.log`, and the part of each server log the run
produced (`primary-server.log`, `standby-server.log`) with what it found in it
(`*-server.anomalies`, `*-server.errors`).  The exit status is 1 if anything
FAILed.

## What runs

**The data** (`schema.sql`).  `soak.t`, a plain table with lion indexes on
columns of 2, 20 (skewed), 200 (int2), 20k (int8, loaded in runs of 15 rows,
so its containers start as runs) and about a million values, a nullable column
(20% NULL), a `text[]` column (NULL, empty and one to three tags), a
two-column index, and a btree primary key; indexes built after the load (the
build path).  `soak.p`, the same columns hash-partitioned three ways, its
indexes created before the load (the insert path).  `soak.d`, a small
dimension for the FK-side and semi/anti joins.  The heaps are at fillfactor 90,
so updates of the column no index reads are HOT where they fit.

**Writers** (`writers/*.sql`, pgbench, `--writers` clients, both tables, three
quarters of the traffic on `soak.t`), alternating two phases:

- *mixed* (`--mixed` seconds): single-row and multi-row inserts, updates of
  indexed columns (one row, or a run of ten), HOT updates, deletes, a
  multi-statement transaction that commits, one that rolls back, a savepoint
  rolled back inside a committing transaction, `INSERT ... ON CONFLICT DO
  UPDATE`, and racing `ON CONFLICT DO NOTHING` inserts of a narrow band of ids
  (two writers inserting the same id at once leave a killed speculative tuple);
- *abort* (`--abort` seconds, `--abort-rate` transactions a second): only
  writes that roll back (a ROLLBACK, or a ROLLBACK TO SAVEPOINT in a
  transaction that commits nothing else).  With the VACUUM loop running back
  to back the heap keeps coming back to all-visible, which is the state the
  aggregates over keys (§37) trust the entries' own counts in - and an aborted
  write is the one change VACUUM may undo under a snapshot that holds it back
  (`findings/wagg_aborted_insert.spec`; fixed by PR #15,
  `claude/lion-wagg-aborted`).

pgbench retries deadlocks (`--max-tries`); any other error a client stops on is
a FAIL.

**VACUUM**: plain `VACUUM` of both tables, every third pass `VACUUM
(INDEX_CLEANUP ON)` (a plain VACUUM of a few dead rows skips the indexes and
leaves the pages unmarked), every fifth a parallel one, every seventh `VACUUM
(FREEZE, INDEX_CLEANUP ON)`, now and then with ANALYZE; two seconds apart in
the mixed phase, back to back on `soak.t` in the abort phase.

**Readers** (`checks.sql`, `--readers` loops on the primary, as many on the
standby).  Each iteration opens a REPEATABLE READ transaction - SERIALIZABLE a
quarter of the time on the primary - and runs one of:

- a round of random queries of both tables (`soak.round()`);
- a long transaction: a round, eight to eighteen seconds of sleep (VACUUMs run
  meanwhile), and another round in the same snapshot;
- a cursor read part way and paused for seconds before the rest
  (`soak.cursor_check()`): a GROUP BY, a `count(DISTINCT)`, an ordered or
  bitmap fetch, whose node holds what it holds while VACUUM comes and waits;
- in the abort phase, half the time, a round of the aggregates over keys only.

Every query is run by `soak.cmp()` twice under the transaction's one
snapshot: the **lion side** - `pg_lion.enable_count_pushdown`, LionOrdered and
the semi join on, `enable_seqscan = off`, and random settings that steer the
plan (`work_mem` of 64 kB or 1 MB, which makes counts batch their rechecks and
GROUP BYs spill; the decoded walk, the top k, the lazy set, the probes and the
plain scan switched off now and then; bitmap and index scans off, which leaves
the pushdown nothing to lose to; sorts, hash aggregates and core's join
methods off, one or all of them; on a build with DESIGN.md §39's units,
`pg_lion.pushdown_margin`, each `pg_lion.<kind>_rate`,
`pg_lion.resident_page_cost` and `effective_cache_size` now and then at a
bound or between, which chooses lion's paths where they are never chosen by
default and declines them where they always are; a parallel plan) - and the
**reference**: no lion path at all, `enable_bitmapscan`, `enable_indexscan`
and `enable_indexonlyscan` off, a sequential scan, serial.  The two answers are
compared as multisets of rows; the direct SQL counts are compared with the
query they stand for.  The families:

| family | what the lion side runs |
|---|---|
| eq, in, range, ne, null | `count(*)` with `=`, `IN` (2-6 values, or 20-60), `BETWEEN`/`<`/`>=`, `<>`, `IS [NOT] NULL` |
| and, or | ANDs and ORs across columns, with lists, ranges and tag clauses |
| tags | `@>` and `&&` on the text[] column |
| group1, group2, group3 | `GROUP BY` one, two (nested loop or decoded walk) and three columns, some with a WHERE or a HAVING |
| distinct | `count(DISTINCT)`, alone, with a WHERE, per group |
| all | `count(*)` of every row (§35) |
| keyaggs | `sum`/`avg`/`min`/`max`/`bool_or`/`bool_and` over columns and expressions of one column, no WHERE (§37) |
| topk | `GROUP BY ... ORDER BY count(*) DESC LIMIT k`: without a tie-breaker only the counts are selected, which ties cannot change |
| direct | `lion_index_count()` (one and two keys), `lion_index_count_any()`, `lion_index_count_stats()`, `lion_index_count_group_stats()` (a partition's index on `soak.p`) |
| ordered | `ORDER BY id LIMIT k` over the btree beside a lion filter, and a lion column's own walk |
| fetch | bitmap and plain scans that fetch rows |
| fkjoin | the FK-side join, its GROUP BY, the semi and the anti join |
| twocol | the two-column index's second column |
| cursor | the paused cursors above |
| stats | `lion_index_stats()`: a scalar index's `ntids` is at least the rows the same snapshot counts (equal, at the end, on a still table) |

`summary.txt` counts the checks by family and by **what answered the lion
side**, read from its plan: `count` (LionCount), `keyaggs-headers` /
`keyaggs-counted` (the aggregates over keys from the entries' own counts, or
each entry counted; from an EXPLAIN ANALYZE in the same snapshot just before
the compared run), `ordered`, `semijoin`, `bitmap`, `plain` (a lion index's
plain or index-only scan), or `none` (the planner took no lion path, which
still checks the reference against itself and says how much of the run was
spent there).

**Verify** (`--verify-every` seconds): `lion_index_verify(idx, true)` of every
lion index, partitions' included, on the primary and on the standby, and
`soak.stats_check()`.

**At the end**: the load stops, `VACUUM (FREEZE, INDEX_CLEANUP ON, ANALYZE)`,
the standby catches up, and on each node a verify pass, a sweep of 180 queries
of `soak.t` and 60 of `soak.p`, a cursor, and the exact `ntids` check.  Then
the server logs are scanned.

## What is a FAIL

- a lion answer that differs from the reference in the same snapshot
  (`failures.log` has the rows only one side returned, both row counts, the
  snapshot, the isolation level, the random settings, both queries and the lion
  side's plan);
- any ERROR other than a serialization failure, a deadlock or a recovery
  conflict - in a reader, a writer, VACUUM or verify;
- any WARNING from `lion_index_verify()`, or from anything else in the server
  log;
- an assertion failure (`TRAP:`), a PANIC, a backend killed by a signal, a
  sanitizer report, a lost connection;
- the standby not catching up within ten minutes at the end;
- `soak.cmp()` called outside a REPEATABLE READ or SERIALIZABLE transaction
  (each statement would have its own snapshot: a harness error, never a pass).

A FAIL found this way is reduced, where it can be, to a deterministic
reproducer in `findings/`.

## cost_bounds.sql

Not a soak: on a build with DESIGN.md §39's units and margin, 26 queries -
counts, GROUP BYs, count(DISTINCT), the top k, a multi-key column, a
partitioned table, the FK-side, semi and anti joins, LATERAL joins,
LionOrdered, six of them as generic plans - each planned under 13 settings
from the defaults to every rate and the margin at their bounds, with core's
joins, aggregates or scans all off, and in parallel, and each answer compared
with the same query's with lion's paths off.  It prints every plan's lion
node and cost and a verdict line; a differing answer or a cost that is not
finite fails.  A build without the settings skips it.

    eval "$(./dev.sh env)"; psql -X -f test/soak/cost_bounds.sql

## findings/

One file per finding: an isolation spec (parked at an injection point, so the
race is pinned, not hoped for) or a SQL script, each with a header saying what
is wrong, where, and what the correct answer is.  The expected output of a
spec holds the CORRECT answer, so the spec fails while the bug is there:

    eval "$(./dev.sh env)"
    test/soak/findings/run-spec.sh                       # all of them
    test/soak/findings/run-spec.sh wagg_aborted_insert   # one

The specs need a server built with `--enable-injection-points` and the
`injection_points` module, as the injection-point specs of `test/isolation`
do; they run in a database of their own (`lion_findings`).

## Limits

- The reference is PostgreSQL's own sequential scan in the same snapshot; a
  bug that changes what the heap holds (a lost row) is caught only by
  `heapallindexed` verify, not by a comparison.
- A race the soak hits once in ten minutes it may miss the next time: a clean
  run is evidence, not proof.  Some it practically never hits: the aggregates
  over keys count an aborted row only if a whole VACUUM cycle - the pruning,
  every index's bulk delete, the second heap pass - fits between the walk
  reading the entry and its second look at the map, and VACUUM's pass over
  the very index the walk is reading follows the walk at much the same pace.
  The soaks of 2026-10-01 took that path about a thousand times without a
  wrong answer; `findings/wagg_aborted_insert.spec` parks the walk and gets
  one every time.  That is what the injection-point specs are for.  (PR #15,
  `claude/lion-wagg-aborted`, fixes it: the walk reads a count of finished
  `ambulkdelete` calls, kept in the meta page, before its first look at the
  map and after its second, and counts exactly if it moved; the spec becomes
  a `test/isolation` spec there.)  The rounds are seeded (`--seed`,
  `setseed()` per reader iteration), but thread scheduling is not
  reproducible; what is, is the finding's spec.
- No posting set here is a BITSET.  The rows are about 40 to a heap page, so
  a container key's 64 pages hold about 2,500 rows, and even `c2`'s half of
  them stays under the 4,096 members a BITSET needs: the dense sets are
  ARRAYs (and, with the NARROW container of the narrow-bitset branch,
  NARROWs).  The BITSET arms of the set algebra, the readers, the inserts and
  VACUUM are the regression suite's to test, on fixtures of narrow rows, not
  this soak's; the summary's container counts at the end of each run show
  it.
- The dev cluster runs `synchronous_commit = off`: a page whose newest commit
  is not yet flushed cannot be marked all-visible (core sets no hint bit for
  it), which delays the all-visible state the abort phase aims at by up to
  `wal_writer_delay`.
- Wall clock and CPU: the default run is four pgbench clients at several
  thousand transactions a second and two readers; on a shared machine scale
  `--writers` and `--readers` down rather than the duration.
