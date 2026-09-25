# Crash-recovery and hot-standby tests

`pg_lion` writes generic WAL (`GenericXLogStart`/`GenericXLogFinish`,
DESIGN.md §4–§5), and the count path treats a standby specially: in recovery
the pin interlock of §9 does not hold, so the count trusts the visibility map
for nothing and rechecks every candidate TID. Neither of those had an
automated test. The only recovery check the project ever had was one manual
`pg_ctl -m immediate` early in development, before sparse segments (§13),
byte-sized bucket counts, the reserved NULL and EMPTY entries (§14, §17),
multi-key operator classes (§17) and the reworked two-pass VACUUM (§11)
existed. This directory is that test.

## Running it

```sh
make recovery-check PG_CONFIG=<prefix>/bin/pg_config RECOVERY_PREFIX=<prefix>

# or directly:
test/recovery/run.sh [--prefix <prefix>] [--iters N] [--keep]
```

`<prefix>` is a PostgreSQL *installation* (it needs `bin/initdb`,
`bin/pg_basebackup`, `bin/pgbench`, `bin/pg_waldump`, and `citext` in
`share/postgresql/extension`). It defaults to the worktree install,
`../pg_roaring_index-partial/.local/pg`. The extension is rebuilt from a
clean copy of this tree — `src/`, the Makefile, the control and SQL files,
with `src/*.o` removed — and installed into that prefix, exactly as
`bench/lib.sh`'s `bench_build_extension` does; `make install` is never run in
the source tree.

The run creates two clusters of its own under
`/tmp/claude-1000/lion_recovery`, on the private socket directory
`/tmp/claude-1000/pgsk_rec` and ports 54340 (primary) and 54341 (standby),
with `listen_addresses = ''`. It refuses to start if anything already listens
there, checks `SHOW data_directory` against the directory it asked for before
trusting a connection, and removes both clusters through an exit trap
(`--keep` suppresses only the removal). It never touches the dev cluster or
the benchmark clusters.

One caveat about that prefix: it is shared. The run installs
`pg_lion.so` into it and then starts a cluster that loads it, so another
process running `make install` against the same prefix in that window will
have its build tested instead of yours. That shows up as `lion_index_verify`
failing in the *baseline* check, before any crash has happened — a failure
there is a reason to check who else is installing into the prefix, not a
recovery bug. Every failure after the baseline is about recovery.

Takes about 65-80 s with the default 8 iterations. Full output goes to
`test/recovery/log/run.log`; on failure the two server logs are copied next to
it. `make clean` removes the directory.

## Files

| file | what it is |
| --- | --- |
| `run.sh` | the whole thing: build, clusters, crash loop, standby, promotion, summary |
| `schema.sql` | the fixture and every check, as read-only SQL functions |
| `writer.sql` | the pgbench script the crashes interrupt |
| `standby_rr.sql` | the standby side of the recovery-conflict test |

The checks live in the database rather than in the shell because they have to
run unchanged on the primary, on a hot standby and on a promoted standby, and
a standby can be given neither new functions nor temp tables. So every helper
in `schema.sql` is read-only, and the multiset comparison that the SQL
regression tests spell as two `CREATE TEMP TABLE`s plus `EXCEPT ALL` is done
over two materialised text arrays — still with `EXCEPT ALL` in both
directions.

## What is covered

The fixture is a table with one column per operator class kind — `int4`,
`text`, `citext`, `bool`, a nullable `int4`, `int4[]` (with `NULL`, `'{}'` and
all-`NULL`-element rows) and `tsvector` (with `NULL` and empty rows) — so both
reserved key-less entries are populated. Three of the seven lion indexes
are created on the empty table and filled by `aminsert`, so their entries go
through INLINE→CHAIN spills, sparse-segment promotions and container-page
splits under WAL; four are built by `ambuild` after the load.
`inline_limit = 64` on three of them makes spills and splits frequent rather
than rare, `buckets = 8`/`16` forces bucket-page chains, and the 3000 distinct
values of `t` over ~800 heap pages give about one member per container key,
which is what puts sparse segments in an index at all.

**Phase 1, crash recovery under load.** Eight rounds of: ~2 s of mixed writes
(pgbench, 4 clients, `synchronous_commit = on`, ~430 transactions/s), then kill
the server with no chance to flush — `pg_ctl stop -m immediate` and `kill -9`
of the postmaster, alternating — restart, and check. Each round replays
30000–70000 generic WAL records.

Each writer transaction inserts three new rows, does a non-HOT update of every
indexed column of one of them (so every index gains a TID and loses one),
updates a short primary-key range of its own rows, and deletes its three
lowest-numbered rows; a second session `VACUUM`s every 0.6 s. Three rows in
and three out keeps the table at exactly 40000 rows while the live id range
slides upwards, which is the point: `VACUUM` frees low heap pages that later
inserts reuse, so container keys arrive out of order and the rarer placement
paths get exercised — a pair landing inside a *full* sparse segment (which
§13 notes only happens for out-of-order inserts) and a container inserted in
the middle of a chain page rather than appended at the tail. Each pgbench
client works only on rows with `id % 8 = client_id`, so no two clients contend
for a row and a deadlock cannot abort the load.

Rounds 5 and 6 crash 350 ms into a foreground `VACUUM`, so replay has to
finish a half-applied two-pass bulkdelete. After every restart:

* the restart really did recover: the log must say the system was not properly
  shut down and must print `redo starts at`, and `pg_waldump -r Generic` over
  the replayed range must have found generic records. A round that replayed
  nothing fails the run instead of passing silently.
* `lion_index_verify(idx, true)` on all seven indexes — structure plus
  every heap tuple present under every key it extracts to.
* every single-key probe counted four ways — forced index path, forced
  sequential scan with no index path at all, `lion_index_count()`, and the
  count pushdown — must give the same number. 19 probes, including absent
  keys, a lower-case `citext` key against mixed-case data, and both `bool`
  values.
* 20 multi-row probes — `GROUP BY` on every column kind including the NULL
  group, the `IS NULL` and `IS NOT NULL` pushdown drivers, `@>`, `&&` and `@@`
  — compared between the index path and a forced sequential scan as multisets,
  `EXCEPT ALL` in both directions.
* `lion_index_stats().ntids` against the heap: `>=` immediately after
  recovery (the index may still hold TIDs of dead tuples), and exactly equal
  after a `VACUUM`. For the two multi-key indexes the expected total is the
  number of (key, row) pairs, with one pair for a row that extracted no keys.
  `null_tids` and `empty_tids` are checked separately, because a reserved
  entry that lost its flag would hide inside the `ntids` total.

**Phases 1b-1e, a crash at a chosen point.** Each builds a
table of its own. 1b parks a VACUUM between the two steps of a whole-chain
free (injection point `lion-vacuum-entries-deleted`), 1c and 1d an insert
between a directory or posting-tree split and its downlink, and each crashes
the server there and checks what comes back. One more (DESIGN.md §18):

* **1e**, a crash inside a MULTI-LEAF spill. VACUUM's filtering turns one
  key's INLINE payload into ten BITSETs, a leaf each; the spill parks at
  `lion-spill-leaves-written`, after its last leaf record and before the root
  and the entry, and the server dies. The entry must come back INLINE with
  every TID, `lion_index_verify()` must call the ten leaves a leak and not
  corruption, and the next VACUUM must spill the set and free the orphans in
  its sweep. A second crash replays that VACUUM.

**Phase 2, hot standby.** `pg_basebackup -R -X stream` into a standby, then:

* the full phase-1 check battery again, in recovery.
* `lion_index_count_stats()` for every single-key probe must show
  `blocks_skipped = 0` and `tids_rechecked = count` — §9's rule that a standby
  never trusts the visibility map. On the primary the same probes must show
  the opposite (blocks skipped via the map), or the standby assertion would be
  vacuously true.
* a line-per-probe picture of everything the index answers — each key count
  through `lion_index_count()`, each multi-row query through the pushdown,
  with the plan node that answered it — must be byte-identical between primary
  and standby. Comparing the node too means a standby that quietly stopped
  using the pushdown is a failure rather than a silently weaker test.
* the recovery-conflict case, run twice. A standby session opens a
  `REPEATABLE READ` transaction and counts key K; the run waits (on
  `pg_stat_activity`, not on a timer) until that session really holds its
  snapshot, then deletes K's rows on the primary and vacuums. With
  `hot_standby_feedback = on` the primary must not have been able to remove
  the TIDs (asserted against `ntids`) and the session's next count of K must
  be the same number. With it off the removal must have happened (also
  asserted) and `max_standby_streaming_delay = 2s` makes the conflict
  reachable: the only legal outcomes are the same number again or a cancelled
  session with `conflict with recovery` on stderr. A *different* number fails
  the run. Afterwards, once the standby has caught up, a fresh session must
  see the new count, through `lion_index_count()` and through a sequential
  scan.
* `pg_ctl promote`, then `lion_index_verify(idx, true)` and the whole
  battery again on the promoted node, exact `ntids` after a `VACUUM`, and
  `lion_index_count_stats()` showing it trusting the visibility map again.

**Phase 3, a standby reader holding its pin** (DESIGN.md §9, §11, §25). A
standby of its own (phase 2 promoted the first one), with
`max_standby_streaming_delay = -1`, and three cases, each of which parks a
standby `lion_index_count()` at the injection point
`lion-count-containers-pinned` - containers copied, page pinned, visibility map
not yet consulted - and then has the primary move the TIDs it copied off that
page and VACUUM them there:

* **split**: the reader pins the leftmost leaf of a posting tree; inserts grow
  that leaf's first container until it splits, which moves the containers
  holding dead TIDs to the new right sibling, and the VACUUM removes them
  there. The primary's VACUUM cleanup-locks the pinned leaf but writes nothing
  for it. Runs with `pg_lion.vacuum_barrier_ranges = 1`, so the barrier goes
  out in stand-alone VACUUM_VISIT records.
* **pushdown**: the reader pins a one-page posting set's root; an insert
  pushes it down and the VACUUM cleans the child.
* **spill**: the reader pins a directory leaf for an INLINE set; the VACUUM's
  own filtering makes the payload outgrow the entry and spill.

The run then waits until replay is either BLOCKED (the startup process waiting
for a cleanup lock) or has replayed past the primary's last record, releases
the reader and compares its count with the one its snapshot must see. In rmgr
mode replay must have blocked; in generic mode it catches up and the count must
still be right (the standby rechecks every TID). A failing case is reported and
the other cases still run. `PHASE3_CASES="split spill"` in the environment runs
a subset, and `--phases 3` skips phases 1 and 2.

## What is not covered

* **No torn-page test.** Nothing here interrupts a page write in the middle.
  Generic WAL records carry a delta against a page image and full-page writes
  are on, so a torn page is PostgreSQL's problem rather than the AM's, but
  that is an argument, not a test. Doing it properly wants a filesystem or
  device layer that can fail a write at an offset. The clusters do run with
  `--data-checksums`, so a page that replay corrupted structurally *inside*
  the page would be caught by the reads the checks do.
* **No long soak.** Eight rounds of two seconds is about 20000 transactions;
  it is a smoke test for the WAL records the write paths emit, not a
  statement about hours of churn. `--iters N` raises it.
* **Crash injection at a few chosen points only.** Phases 1b-1e stop a
  backend at an injection point and crash the server there; every other
  multi-record operation (a segment promotion, say) is crashed wherever the
  phase 1 workload happens to be, which reaches the frequent paths and not
  the rare ones.
* **No replica that is behind.** The standby is always caught up before it is
  read, except in the conflict test where being behind is the point. Nothing
  tests a cascading standby, a restart from an archive, `pg_rewind`, or a
  timeline switch other than the one `pg_ctl promote` makes.
* **No unlogged or temporary indexes.** `test/sql/unlogged.sql` covers what an
  unlogged lion index does; recovery truncates it, so there is nothing to
  check here.
* **No concurrent readers during the crash.** The writers are killed; nobody
  is mid-scan. The interlock between readers and VACUUM is what
  `test/isolation/` is for.
