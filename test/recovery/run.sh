#!/bin/bash
#
# pg_lion: automated crash-recovery and hot-standby tests.
#
# USAGE
#
#   test/recovery/run.sh [--prefix <pg install prefix>] [--iters N] [--keep]
#   make recovery-check PG_CONFIG=<prefix>/bin/pg_config RECOVERY_PREFIX=<prefix>
#
#   --prefix   PostgreSQL installation to use (bin/, lib/, share/).  Defaults
#              to the worktree install, ../pg_roaring_index-partial/.local/pg.
#              The extension is rebuilt from a clean copy of this tree and
#              installed into that prefix; the tree itself is never written to
#              and "make install" is never run in it.
#   --iters    crash/recover iterations in phase 1 (default 8).
#   --keep     leave the clusters and their logs in place on exit.
#   --mode     generic (the default) or rmgr: rmgr preloads the library so
#              that the custom WAL resource manager of DESIGN.md §25 is
#              registered and every index built here is written through it.
#              Phase 2 then expects the standby to TRUST the visibility map,
#              which is the property §25 buys.
#   --phases   which phases to run, e.g. "3" or "2 3" (default: all of
#              "1 1b 1c 1d 2 3").  For development; `make recovery-check`
#              always runs everything.
#   --conf     an extra postgresql.conf line for the primary (repeatable),
#              appended last so it wins.  This is how
#              wal_consistency_checking = 'pg_lion' is turned on:
#                  --mode rmgr --conf "wal_consistency_checking = 'pg_lion'"
#              which makes the standby (and every crash recovery in phase 1)
#              compare the page replay produced against the page the primary
#              had, for every page of every lion record.
#
# Exit status is 0 only if every check passed.  A summary is printed at the
# end; the full log is written to test/recovery/log/run.log, and on failure the
# server logs of both clusters are copied next to it.
#
# WHAT IT DOES
#
#   Phase 1  N rounds of: run a mixed write load (pgbench, 4 clients,
#            synchronous_commit = on) for ~2 s against a table with one column
#            per operator class kind, kill the server without a chance to
#            flush (pg_ctl -m immediate and kill -9, alternating; twice while a
#            VACUUM is in flight), restart, and check that the indexes came
#            back intact: lion_index_verify(idx, true) for each, single-key
#            counts through the index / a forced seqscan / lion_index_count()
#            / the count pushdown all equal, GROUP BY results equal as
#            multisets (EXCEPT ALL both ways), and lion_index_stats().ntids
#            consistent with the heap - exactly, after a VACUUM.  Each round
#            also proves the restart really replayed generic WAL.
#
#   Phase 2  pg_basebackup -R a hot standby, and check that it answers exactly
#            what the primary answers, that lion_index_count_stats() shows
#            it trusting the visibility map for nothing (DESIGN.md section 9,
#            "Hot standby"), and that a REPEATABLE READ standby snapshot whose
#            rows the primary deletes and vacuums is either preserved or
#            cancelled with a recovery conflict - never silently answered with
#            a different number.  Then promote it and verify every index again.
#
# See test/recovery/README.md for what is deliberately not covered.
#
# The cluster discipline is bench/lib.sh's: a private socket directory and port
# that nothing else uses, refuse to start if either is already in use, check
# SHOW data_directory before the first statement, and remove everything on exit
# through a trap.  These clusters are created here and are never shared with
# the dev cluster or the benchmark clusters.

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT=$(cd "$HERE/../.." && pwd)

PREFIX=${RECOVERY_PREFIX:-}
if [ -z "$PREFIX" ]; then
	PREFIX=$(cd "$PROJECT/../pg_roaring_index-partial/.local/pg" 2>/dev/null && pwd || true)
fi
ITERS=8
KEEP=0
MODE=generic
PHASES="1 1b 1c 1d 2 3"
EXTRA_CONF=()

BASE=/tmp/claude-1000/lion_recovery
SOCKDIR=/tmp/claude-1000/pgsk_rec
PRIMARY_PORT=54340
STANDBY_PORT=54341
DBNAME=postgres

# Endpoints that belong to other runs (the dev cluster, the benchmark
# clusters).  Hard-refuse rather than trust the variables above.
FORBIDDEN_PORTS="54329 54330 54331 54332"
FORBIDDEN_SOCKDIRS="/tmp/claude-1000/pgsk /tmp/claude-1000/pgsk_bench /tmp/claude-1000/pgsk_wt"

LOGDIR=$HERE/log
RUNLOG=$LOGDIR/run.log

PRIMARY_DATA=$BASE/primary
STANDBY_DATA=$BASE/standby
PRIMARY_LOG=$BASE/primary.log
STANDBY_LOG=$BASE/standby.log
BUILDDIR=""
VACLOOP=""
VACPID=""
RRPID=""
GENERIC_TOTAL=0
SUMMARY=()
PHASE3_FAILS=()

nap() { command sleep "$1"; }
die() { echo "FAIL: $*" >&2; exit 1; }
log() { echo "$*" | tee -a "$RUNLOG"; }
now_ms() { date +%s%3N; }

# ---------------------------------------------------------------- arguments

while [ $# -gt 0 ]; do
	case $1 in
		--prefix) PREFIX=${2:-}; shift 2 ;;
		--iters)  ITERS=${2:-}; shift 2 ;;
		--keep)   KEEP=1; shift ;;
		--mode)   MODE=${2:-}; shift 2 ;;
		--conf)   EXTRA_CONF+=("${2:-}"); shift 2 ;;
		--phases) PHASES=${2:-}; shift 2 ;;
		-h|--help) sed -n '3,20p' "${BASH_SOURCE[0]}" | sed 's/^#\{0,1\} \{0,1\}//'; exit 0 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

case $MODE in
	generic) ;;
	rmgr)
		# DESIGN.md §25: the custom resource manager can only be registered
		# from shared_preload_libraries, and an index built on such a server
		# is written through it.
		EXTRA_CONF+=("shared_preload_libraries = 'pg_lion'")
		;;
	*) die "--mode wants generic or rmgr" ;;
esac

[ -n "$PREFIX" ] || die "no --prefix given and the worktree install was not found"
PREFIX=$(cd "$PREFIX" 2>/dev/null && pwd) || die "prefix does not exist"
PGBIN=$PREFIX/bin
for prog in initdb pg_ctl psql pgbench pg_basebackup pg_waldump pg_config; do
	[ -x "$PGBIN/$prog" ] || die "$PGBIN/$prog is missing; is $PREFIX a PostgreSQL install?"
done
case $ITERS in ''|*[!0-9]*) die "--iters wants a number" ;; esac
[ "$ITERS" -ge 1 ] || die "--iters must be at least 1"

# ---------------------------------------------------------------- safety

for p in $FORBIDDEN_PORTS; do
	[ "$PRIMARY_PORT" = "$p" ] && die "port $p belongs to another cluster"
	[ "$STANDBY_PORT" = "$p" ] && die "port $p belongs to another cluster"
done
for d in $FORBIDDEN_SOCKDIRS; do
	[ "$SOCKDIR" = "$d" ] && die "socket directory $d belongs to another cluster"
done
case $BASE in
	/tmp/claude-1000/lion_recovery) ;;
	*) die "refusing to manage data directories outside /tmp/claude-1000/lion_recovery" ;;
esac
if [ -S "$SOCKDIR/.s.PGSQL.$PRIMARY_PORT" ] || [ -S "$SOCKDIR/.s.PGSQL.$STANDBY_PORT" ]; then
	die "something already listens on $SOCKDIR:$PRIMARY_PORT/$STANDBY_PORT; refusing to share the endpoint"
fi

# ---------------------------------------------------------------- shutdown

stop_hard() {
	local d=$1
	[ -d "$d" ] || return 0
	"$PGBIN/pg_ctl" -D "$d" stop -m immediate -w -t 30 >/dev/null 2>&1
	return 0
}

kill_tree() {
	local pid=$1 k
	[ -n "$pid" ] || return 0
	kill -0 "$pid" 2>/dev/null || return 0
	for k in $(pgrep -P "$pid" 2>/dev/null || true); do kill -9 "$k" 2>/dev/null || true; done
	kill -9 "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	return 0
}

cleanup() {
	local rc=$?
	trap - EXIT
	set +e
	kill_tree "$RRPID"
	kill_tree "$VACPID"
	kill_tree "$VACLOOP"
	stop_hard "$STANDBY_DATA"
	stop_hard "$PRIMARY_DATA"
	if [ "$rc" != 0 ]; then
		mkdir -p "$LOGDIR"
		for f in "$PRIMARY_LOG" "$STANDBY_LOG"; do
			[ -f "$f" ] && cp "$f" "$LOGDIR/$(basename "$f")"
		done
		echo "server logs copied to $LOGDIR" >&2
	fi
	if [ "$KEEP" = 1 ]; then
		echo "--keep: clusters left in $BASE (prefix $PREFIX)"
	else
		[ -n "$BUILDDIR" ] && rm -rf "$BUILDDIR"
		rm -rf "$BASE"
		rmdir "$SOCKDIR" >/dev/null 2>&1
	fi
	exit $rc
}

# ---------------------------------------------------------------- psql

psql_p() {
	"$PGBIN/psql" -X -q -v ON_ERROR_STOP=1 -h "$SOCKDIR" -p "$PRIMARY_PORT" \
		-U postgres -d "$DBNAME" "$@"
}
psql_s() {
	"$PGBIN/psql" -X -q -v ON_ERROR_STOP=1 -h "$SOCKDIR" -p "$STANDBY_PORT" \
		-U postgres -d "$DBNAME" "$@"
}

# wait_true <psql fn> <boolean sql> <seconds> <what>
wait_true() {
	local fn=$1 sql=$2 limit=$3 what=$4 i=0 v=""
	while [ "$i" -lt "$((limit * 10))" ]; do
		v=$("$fn" -tAc "$sql" 2>/dev/null || true)
		[ "$v" = "t" ] && return 0
		i=$((i + 1))
		nap 0.1
	done
	die "timed out after ${limit}s waiting for: $what"
}

# ---------------------------------------------------------------- build

build_extension() {
	log "-- building the extension into $PREFIX"
	BUILDDIR=$(mktemp -d /tmp/claude-1000/lion_rec_build.XXXXXX)
	cp -r "$PROJECT/src" "$PROJECT/Makefile" "$PROJECT"/pg_lion*.control \
		"$PROJECT"/pg_lion*--*.sql "$BUILDDIR/"
	# Never reuse objects built against another server.
	rm -f "$BUILDDIR"/src/*.o "$BUILDDIR"/src/*.bc "$BUILDDIR"/*.so
	mkdir -p "$BUILDDIR/test/sql" "$BUILDDIR/test/isolation"
	(
		cd "$BUILDDIR" &&
		make -s PG_CONFIG="$PGBIN/pg_config" &&
		make -s PG_CONFIG="$PGBIN/pg_config" install
	) >>"$RUNLOG" 2>&1 || die "extension build failed (see $RUNLOG, build dir $BUILDDIR)"
}

# ---------------------------------------------------------------- clusters

write_primary_conf() {
	cat >>"$PRIMARY_DATA/postgresql.conf" <<-EOF

		# ---- test/recovery/run.sh
		listen_addresses = ''
		port = $PRIMARY_PORT
		unix_socket_directories = '$SOCKDIR'
		# The crash tests are about durability: a commit pgbench saw must be on
		# disk before the server is taken away.
		fsync = on
		synchronous_commit = on
		wal_level = replica
		max_wal_senders = 5
		max_wal_size = 1GB
		# Enough that the range each restart replays is still on disk when
		# recovery_evidence() runs pg_waldump over it.
		wal_keep_size = 512MB
		hot_standby = on
		# Explicit VACUUMs only, so the crash points and the recovery-conflict
		# test are the ones this script chose.
		autovacuum = off
		max_connections = 30
		# So pg_lion.enable_count_pushdown exists in every session,
		# including the ones the read-only helpers run in on a standby.
		session_preload_libraries = 'pg_lion'
		log_line_prefix = '%m [%p] '
		log_checkpoints = on
	EOF

	# --mode and --conf, last so that they win.
	local line
	for line in "${EXTRA_CONF[@]}"; do
		printf '%s\n' "$line" >>"$PRIMARY_DATA/postgresql.conf"
	done
}

write_standby_conf() {
	local feedback=$1 delay=$2
	cat >"$STANDBY_DATA/standby_extra.conf" <<-EOF
		# ---- test/recovery/run.sh (included last, so it wins)
		hot_standby = on
		hot_standby_feedback = $feedback
		max_standby_streaming_delay = $delay
		# The default 10s would make the feedback-on case depend on luck.
		wal_receiver_status_interval = 1s
	EOF
}

start_node() {
	local d=$1 port=$2 logf=$3 tries=0
	while :; do
		if "$PGBIN/pg_ctl" -D "$d" -l "$logf" \
			-o "-p $port -k $SOCKDIR -c listen_addresses=''" -w -t 120 start \
			>>"$RUNLOG" 2>&1; then
			break
		fi
		tries=$((tries + 1))
		[ "$tries" -ge 3 ] && die "could not start the cluster at $d (see $logf)"
		# A SIGKILLed postmaster can leave a backend attached for a moment.
		nap 1
	done
}

verify_node() {
	local fn=$1 d=$2 actual
	actual=$("$fn" -tAc "show data_directory" 2>/dev/null || true)
	[ -n "$actual" ] || die "could not ask the cluster at $d which data directory it serves"
	[ "$(cd "$actual" 2>/dev/null && pwd)" = "$(cd "$d" && pwd)" ] ||
		die "connected to a cluster with data_directory '$actual', expected '$d'"
}

init_primary() {
	log "-- initdb $PRIMARY_DATA"
	mkdir -p "$BASE" "$SOCKDIR"
	"$PGBIN/initdb" -D "$PRIMARY_DATA" -U postgres --auth=trust --no-sync \
		--data-checksums -E UTF8 --locale=C >>"$RUNLOG" 2>&1 ||
		die "initdb failed (see $RUNLOG)"
	write_primary_conf
	start_node "$PRIMARY_DATA" "$PRIMARY_PORT" "$PRIMARY_LOG"
	verify_node psql_p "$PRIMARY_DATA"
	[ "$(psql_p -tAc 'select pg_is_in_recovery()')" = "f" ] ||
		die "the fresh primary is in recovery"
}

# ---------------------------------------------------------------- checks

NCHECKS=0

# run_check <label> <psql fn> <sql returning (ok boolean, detail text)>
#
# Fails the run on any row with ok = false, and on an empty result (a check
# battery that produced nothing has checked nothing).  Sets NCHECKS.  Never
# call this in a command substitution: it must be able to exit the script.
run_check() {
	local label=$1 fn=$2 sql=$3 out fails rc=0 errf=$BASE/check.err
	out=$("$fn" -tA -F'|' -c "$sql" 2>"$errf") || rc=$?
	{ echo "---- $label"; printf '%s\n' "$out"; echo "---- $label stderr"; cat "$errf"; } >>"$RUNLOG"
	grep '^WARNING' "$errf" >>"$BASE/warnings.txt" 2>/dev/null || true
	[ "$rc" = 0 ] || die "$label: the check query itself failed: $(tail -3 "$errf")"
	fails=$(printf '%s\n' "$out" | grep -v '^t|' || true)
	if [ -n "$fails" ]; then
		echo "FAILED checks ($label):" >&2
		printf '%s\n' "$fails" >&2
		die "$label: $(printf '%s\n' "$fails" | wc -l) check(s) failed (full log: $RUNLOG)"
	fi
	NCHECKS=$(printf '%s\n' "$out" | wc -l)
}

# ---------------------------------------------------------------- phase 1

crash_immediate() {
	"$PGBIN/pg_ctl" -D "$PRIMARY_DATA" stop -m immediate -w -t 60 >>"$RUNLOG" 2>&1 ||
		die "pg_ctl stop -m immediate failed"
}

crash_kill9() {
	local pm kids k i=0
	pm=$(head -1 "$PRIMARY_DATA/postmaster.pid")
	[ -n "$pm" ] || die "no postmaster.pid to kill"
	kids=$(pgrep -P "$pm" 2>/dev/null | tr '\n' ' ' || true)
	kill -9 "$pm" 2>/dev/null || true
	# Backends normally exit on their own when the postmaster dies
	# (WL_EXIT_ON_PM_DEATH); make sure none is left attached to the shared
	# memory segment, or the restart would refuse to run.
	for k in $kids; do kill -9 "$k" 2>/dev/null || true; done
	while kill -0 "$pm" 2>/dev/null; do
		i=$((i + 1))
		[ "$i" -gt 300 ] && die "postmaster $pm survived SIGKILL"
		nap 0.1
	done
}

# Everything the log says about the recovery that just happened, so an
# iteration that replayed nothing cannot pass silently.  Prints
# "<redo start LSN> <generic WAL records replayed>".
recovery_evidence() {
	local off=$1 txt redo_start redo_end generic
	txt=$(tail -c "+$((off + 1))" "$PRIMARY_LOG")
	printf '%s\n' "$txt" >>"$RUNLOG"
	printf '%s' "$txt" | grep -q "database system was not properly shut down" ||
		die "the restart did not report an unclean shutdown: the crash did not take effect"
	redo_start=$(printf '%s' "$txt" |
		sed -n 's/.*redo starts at \([0-9A-F]*\/[0-9A-F]*\).*/\1/p' | head -1)
	[ -n "$redo_start" ] ||
		die "the restart replayed no WAL ('redo starts at' is missing): nothing was tested"
	redo_end=$(printf '%s' "$txt" |
		sed -n 's/.*redo done at \([0-9A-F]*\/[0-9A-F]*\).*/\1/p' | head -1)
	[ -n "$redo_end" ] ||
		die "the restart printed no 'redo done at': recovery did not finish normally"
	# The write paths of this AM emit nothing but records of ONE resource
	# manager, so a range that replayed none of them replayed no index change
	# at all.  Both names are counted because which one it is depends on
	# --mode: generic WAL is "Generic", and in rmgr mode pg_waldump prints
	# "customNNN", since it does not load the module and cannot ask it for a
	# name (DESIGN.md §25).
	#
	# pg_waldump reads the range off DISK, so a range longer than
	# wal_keep_size is no longer there to be counted; that is a limit of the
	# EVIDENCE, not of the test, so it is reported and skipped rather than
	# treated as a failure.  wal_consistency_checking makes every record carry
	# a full-page image and is what makes a range that long, so
	# recovery-check-rmgr raises wal_keep_size to match.
	generic=$("$PGBIN/pg_waldump" -p "$PRIMARY_DATA/pg_wal" \
		-s "$redo_start" -e "$redo_end" 2>/dev/null |
		grep -cE 'rmgr: (Generic|custom[0-9]+)' || true)
	if [ "$generic" -eq 0 ]; then
		if "$PGBIN/pg_waldump" -p "$PRIMARY_DATA/pg_wal" -s "$redo_start" \
			-e "$redo_end" >/dev/null 2>&1; then
			die "the range $redo_start..$redo_end was replayed but holds no index WAL record: this round tested no index change"
		fi
		log "note: $redo_start..$redo_end is no longer on disk (wal_keep_size); the replay happened, the count did not"
	fi
	echo "$redo_start $generic"
}

start_vacuum_loop() {
	(
		while :; do
			"$PGBIN/psql" -X -q -h "$SOCKDIR" -p "$PRIMARY_PORT" -U postgres \
				-d "$DBNAME" -c "VACUUM (INDEX_CLEANUP ON) lion_rec" >/dev/null 2>&1 || true
			command sleep 0.6
		done
	) &
	VACLOOP=$!
}

stop_vacuum_loop() {
	kill_tree "$VACLOOP"
	VACLOOP=""
}

phase1() {
	local it style vacuum_crash off ev t0 t1 nrows ck
	log ""
	log "=== phase 1: crash recovery under load ($ITERS iterations) ==="
	for it in $(seq 1 "$ITERS"); do
		t0=$(now_ms)
		if [ $((it % 2)) -eq 1 ]; then style=immediate; else style=kill9; fi
		vacuum_crash=0
		case $it in 5|6) vacuum_crash=1 ;; esac

		if [ "$vacuum_crash" = 0 ]; then start_vacuum_loop; fi
		PGOPTIONS='-c synchronous_commit=on' \
			"$PGBIN/pgbench" -n -r -T 2 -c 4 -j 2 -f "$HERE/writer.sql" \
			-h "$SOCKDIR" -p "$PRIMARY_PORT" -U postgres "$DBNAME" \
			>>"$RUNLOG" 2>&1 || die "iteration $it: the pgbench writer failed (see $RUNLOG)"
		stop_vacuum_loop

		off=$(stat -c %s "$PRIMARY_LOG")
		if [ "$vacuum_crash" = 1 ]; then
			"$PGBIN/psql" -X -q -h "$SOCKDIR" -p "$PRIMARY_PORT" -U postgres \
				-d "$DBNAME" -c "VACUUM (INDEX_CLEANUP ON, VERBOSE) lion_rec" \
				>>"$RUNLOG" 2>&1 &
			VACPID=$!
			nap 0.35
		fi

		"crash_$style"

		if [ "$vacuum_crash" = 1 ]; then
			wait "$VACPID" 2>/dev/null || true
			VACPID=""
		fi

		start_node "$PRIMARY_DATA" "$PRIMARY_PORT" "$PRIMARY_LOG"
		verify_node psql_p "$PRIMARY_DATA"
		[ "$(psql_p -tAc 'select pg_is_in_recovery()')" = "f" ] ||
			die "iteration $it: the primary is still in recovery after pg_ctl -w start"
		ev=$(recovery_evidence "$off")
		GENERIC_TOTAL=$((GENERIC_TOTAL + ${ev#* }))

		# Before VACUUM the index may still hold TIDs of dead tuples, so ntids
		# is only required to be >= what the heap needs; after it, equal.
		run_check "iter $it post-recovery" psql_p "select * from lion_rec_check(true, false)"
		ck=$NCHECKS
		psql_p -c "VACUUM (INDEX_CLEANUP ON) lion_rec" >>"$RUNLOG" 2>&1
		run_check "iter $it post-vacuum" psql_p "select * from lion_rec_check(false, true)"

		nrows=$(psql_p -tAc "select count(*) from lion_rec")
		t1=$(now_ms)
		log "iter $it: crash=$style vacuum_in_flight=$vacuum_crash redo_from=${ev% *} index_wal_replayed=${ev#* } rows=$nrows checks=$((ck + NCHECKS)) $(( (t1 - t0) / 1000 ))s"
		SUMMARY+=("phase1 iter $it  crash=$style vacuum_in_flight=$vacuum_crash index_wal_replayed=${ev#* } rows=$nrows checks=$((ck + NCHECKS))")
	done
	[ "$GENERIC_TOTAL" -gt 0 ] ||
		die "no generic WAL record was ever replayed: the crash phase proved nothing"
}

# ------------------------------------------------------------- phase 1b

# A crash that lands exactly between the two steps of a whole-chain free
# (DESIGN.md §18): the entries are deleted and WAL-logged, their container
# pages are unreachable but not yet marked LION_PAGE_DELETED, and the server
# dies.  The pages are then leaked, which is harmless by construction -
# nothing references them - and the leak sweep at the end of the next
# ambulkdelete is what turns them into free pages again.
#
# The crash point is deterministic: an injection point parks the VACUUM in
# that window and the server is pulled out from under it.  A dedicated table
# is used so that the keys that must empty out are exactly the ones this test
# deletes, and so that nothing of the phase 1 fixture is disturbed.
phase1b() {
	local before after freed parked vacpid off warn
	log ""
	log "=== phase 1b: a crash between entry deletion and page marking ==="

	psql_p -c "CREATE EXTENSION IF NOT EXISTS injection_points" >>"$RUNLOG" 2>&1 ||
		{ log "phase 1b skipped: this server has no injection_points extension"
		  SUMMARY+=("phase1b            skipped (no injection points)"); return 0; }

	psql_p >>"$RUNLOG" 2>&1 <<-SQL || die "phase 1b: fixture failed"
		SET synchronous_commit = on;
		DROP TABLE IF EXISTS lion_leak;
		CREATE TABLE lion_leak (id int, k int NOT NULL);
		CREATE INDEX lion_leak_k ON lion_leak USING lion (k)
			WITH (inline_limit = 64);
		INSERT INTO lion_leak SELECT i, i % 6 FROM generate_series(1, 120000) i;
	SQL
	psql_p -c "VACUUM (ANALYZE) lion_leak" >>"$RUNLOG" 2>&1
	before=$(psql_p -tAc "select container_pages from lion_index_stats('lion_leak_k')")
	psql_p -c "SET synchronous_commit = on; DELETE FROM lion_leak WHERE k IN (0,1,2,3)" \
		>>"$RUNLOG" 2>&1 || die "phase 1b: delete failed"

	# Park the VACUUM in the window and crash the server under it.
	psql_p -c "SELECT injection_points_attach('lion-vacuum-entries-deleted', 'wait')" \
		>>"$RUNLOG" 2>&1 || die "phase 1b: could not attach the injection point"

	off=$(stat -c %s "$PRIMARY_LOG")
	"$PGBIN/psql" -X -q -h "$SOCKDIR" -p "$PRIMARY_PORT" -U postgres -d "$DBNAME" \
		-c "VACUUM (INDEX_CLEANUP ON) lion_leak" >>"$RUNLOG" 2>&1 &
	vacpid=$!

	wait_true psql_p \
		"select count(*) > 0 from pg_stat_activity where wait_event = 'lion-vacuum-entries-deleted'" \
		60 "the VACUUM to park between the entry deletion and the page marking"

	# ambulkdelete's records belong to no transaction, so nothing has flushed
	# them: a crash here would simply lose the whole index vacuum and VACUUM
	# would redo it (correct, but it would test nothing).  A checkpoint makes
	# the first of the two steps durable, so the crash really does land
	# BETWEEN them.
	psql_p -c "CHECKPOINT" >>"$RUNLOG" 2>&1
	parked=$(psql_p -tAc "select entries from lion_index_stats('lion_leak_k')")
	log "phase 1b: parked with $parked of 6 entries left"
	[ "$parked" -lt 6 ] ||
		die "phase 1b: the VACUUM parked before it had deleted an entry"

	crash_immediate
	wait "$vacpid" 2>/dev/null || true

	start_node "$PRIMARY_DATA" "$PRIMARY_PORT" "$PRIMARY_LOG"
	verify_node psql_p "$PRIMARY_DATA"

	# recovery_evidence() is deliberately NOT used here.  The checkpoint above
	# is what makes this crash land between the two steps, and it also leaves
	# redo almost nothing to do; what this round proves is the leak and the
	# sweep, and phase 1 is what proves replay.
	tail -c "+$((off + 1))" "$PRIMARY_LOG" >>"$RUNLOG"
	tail -c "+$((off + 1))" "$PRIMARY_LOG" |
		grep -q "database system was not properly shut down" ||
		die "phase 1b: the restart did not report an unclean shutdown"

	# The entries really are gone, the pages really are leaked (verify()
	# tolerates unreferenced EMPTY container pages, with a WARNING), and the
	# index still answers correctly.
	after=$(psql_p -tAc "select entries from lion_index_stats('lion_leak_k')")
	[ "$after" = "$parked" ] ||
		die "phase 1b: the entry deletion did not survive the crash ($after entries, $parked before)"

	# The pages of the chain that entry owned are now LEAKED: unreferenced,
	# and not yet marked free.  verify() says so (a WARNING, not an error) and
	# the index still answers correctly.
	warn=$(psql_p -c "SELECT lion_index_verify('lion_leak_k', true)" 2>&1 >>"$RUNLOG" |
		grep -c 'unused and unreachable' || true)
	[ "$warn" -gt 0 ] ||
		die "phase 1b: no page was leaked, so the crash did not land between the two steps"
	run_check "phase 1b post-crash" psql_p \
		"select (select entries from lion_index_stats('lion_leak_k')) < 6 as ok,
				format('entries left: %s of 6, %s pages leaked',
					   (select entries from lion_index_stats('lion_leak_k')), $warn) as detail
		 union all
		 select (select count(*) from lion_leak where k = 4) = 20000,
				'k = 4 still answers 20000 rows'
		 union all
		 select (select count(*) from lion_leak where k = 0) = 0,
				'k = 0 answers no rows'"

	# The sweep recovers them: the next ambulkdelete reads the blocks its walk
	# did not account for, turns the leaked ones into DELETED pages and
	# records them in the free space map.  verify() then has nothing left to
	# warn about, which is the assertion that the leak is gone.
	psql_p -c "SET synchronous_commit = on; DELETE FROM lion_leak WHERE k = 4" \
		>>"$RUNLOG" 2>&1
	psql_p -c "VACUUM (INDEX_CLEANUP ON) lion_leak" >>"$RUNLOG" 2>&1
	freed=$(psql_p -tAc "select deleted_pages from lion_index_stats('lion_leak_k')")
	[ "$freed" -gt 0 ] ||
		die "phase 1b: the leak sweep recovered no pages (deleted_pages = $freed)"
	warn=$(psql_p -c "SELECT lion_index_verify('lion_leak_k', true)" 2>&1 >>"$RUNLOG" |
		grep -c 'unused and unreachable' || true)
	[ "$warn" = 0 ] ||
		die "phase 1b: $warn page(s) are still leaked after the sweep"

	psql_p -c "SELECT injection_points_detach('lion-vacuum-entries-deleted')" \
		>>"$RUNLOG" 2>&1 || true
	psql_p -c "DROP TABLE lion_leak" >>"$RUNLOG" 2>&1

	log "phase 1b: the entry deletion survived the crash and the sweep recovered $freed pages (of $before container pages)"
	SUMMARY+=("phase1b            crash between the two steps; sweep recovered $freed pages")
}

# ------------------------------------------------------------- phase 1c

# A crash that lands exactly between the two records of a DIRECTORY SPLIT
# (DESIGN.md §21): the leaf has been split and flagged LION_PAGE_INCOMPLETE_
# SPLIT, its right sibling exists and is linked in, but the parent has no
# downlink for it yet, and the server dies.  Readers never noticed (the right
# link gets them there), and the first writer that descends to that page
# finishes the split before touching it.
#
# The crash point is deterministic: an injection point parks the inserting
# backend in that window and the server is pulled out from under it.
phase1c() {
	local off before after warn parked
	log ""
	log "=== phase 1c: a crash between a directory split and its downlink ==="

	psql_p -c "CREATE EXTENSION IF NOT EXISTS injection_points" >>"$RUNLOG" 2>&1 ||
		{ log "phase 1c skipped: this server has no injection_points extension"
		  SUMMARY+=("phase1c            skipped (no injection points)"); return 0; }

	# A table whose index is created EMPTY and grown by inserts, so that the
	# splits really happen through aminsert and not through ambuild.
	psql_p >>"$RUNLOG" 2>&1 <<-SQL || die "phase 1c: fixture failed"
		SET synchronous_commit = on;
		DROP TABLE IF EXISTS lion_split;
		CREATE TABLE lion_split (id int, k text NOT NULL);
		CREATE INDEX lion_split_k ON lion_split USING lion (k);
		INSERT INTO lion_split SELECT i, 'k' || lpad(i::text, 40, '0')
			FROM generate_series(1, 4000) i;
	SQL
	psql_p -c "VACUUM (ANALYZE) lion_split" >>"$RUNLOG" 2>&1
	before=$(psql_p -tAc "select leaf_pages from lion_index_stats('lion_split_k')")
	[ "${before:-0}" -gt 1 ] ||
		die "phase 1c: the fixture did not split the directory at all"

	psql_p -c "SELECT injection_points_attach('lion-dir-split-incomplete', 'wait')" \
		>>"$RUNLOG" 2>&1 || die "phase 1c: could not attach the injection point"

	off=$(stat -c %s "$PRIMARY_LOG")
	"$PGBIN/psql" -X -q -h "$SOCKDIR" -p "$PRIMARY_PORT" -U postgres -d "$DBNAME" \
		-c "SET synchronous_commit = on;
		    INSERT INTO lion_split SELECT 100000 + i, 'k' || lpad((100000 + i)::text, 40, '0')
		      FROM generate_series(1, 4000) i" >>"$RUNLOG" 2>&1 &
	parked=$!

	wait_true psql_p \
		"select count(*) > 0 from pg_stat_activity where wait_event = 'lion-dir-split-incomplete'" \
		60 "an insert to park between the split and its downlink"

	# The split's record has been inserted but not flushed, and the crash would
	# otherwise simply lose it (and with it the whole split, which would test
	# nothing).  A COMMIT from another session with synchronous_commit = on
	# flushes the WAL stream up to its own commit LSN, which is past the split
	# record.  A CHECKPOINT would be the obvious way and is NOT usable here:
	# the parked backend holds the leaf's content lock EXCLUSIVE and the
	# checkpointer would block writing that very buffer.
	psql_p >>"$RUNLOG" 2>&1 <<-SQL
		SET synchronous_commit = on;
		CREATE TABLE IF NOT EXISTS lion_split_flush (i int);
		INSERT INTO lion_split_flush VALUES (1);
	SQL

	crash_immediate
	wait "$parked" 2>/dev/null || true

	start_node "$PRIMARY_DATA" "$PRIMARY_PORT" "$PRIMARY_LOG"
	verify_node psql_p "$PRIMARY_DATA"

	tail -c "+$((off + 1))" "$PRIMARY_LOG" >>"$RUNLOG"
	tail -c "+$((off + 1))" "$PRIMARY_LOG" |
		grep -q "database system was not properly shut down" ||
		die "phase 1c: the restart did not report an unclean shutdown"

	# The page really came back flagged: verify() says so, as a WARNING and
	# not an error, and the index still answers correctly through the right
	# link that makes an unfinished split invisible to readers.
	warn=$(psql_p -c "SELECT lion_index_verify('lion_split_k')" 2>&1 >>"$RUNLOG" |
		grep -c 'unfinished split' || true)
	[ "$warn" -gt 0 ] ||
		die "phase 1c: no page came back with an unfinished split, so the crash missed the window"
	log "phase 1c: $warn page(s) came back with an unfinished split"
	run_check "phase 1c post-crash" psql_p \
		"select (select count(*) from lion_split where k = 'k' || lpad('1', 40, '0')) = 1,
				'the first key still answers'
		 union all
		 select (select count(*) from lion_split where k = 'k' || lpad('4000', 40, '0')) = 1,
				'the last committed key still answers'"

	# A WRITER's descent is what repairs it.  One insert is enough: the
	# descent that lands on the flagged page finishes its split first.
	psql_p -c "SET synchronous_commit = on;
	           INSERT INTO lion_split SELECT 900000 + i, 'k' || lpad((900000 + i)::text, 40, '0')
	             FROM generate_series(1, 400) i" >>"$RUNLOG" 2>&1 ||
		die "phase 1c: the repairing insert failed"

	warn=$(psql_p -c "SELECT lion_index_verify('lion_split_k', true)" 2>&1 >>"$RUNLOG" |
		grep -c 'unfinished split' || true)
	[ "$warn" = 0 ] ||
		die "phase 1c: $warn page(s) still have an unfinished split after a writer descended"

	after=$(psql_p -tAc "select leaf_pages from lion_index_stats('lion_split_k')")
	run_check "phase 1c repaired" psql_p \
		"select (select count(*) from lion_split where k = 'k' || lpad('900001', 40, '0')) = 1,
				'the repairing insert is indexed'
		 union all
		 select (select count(*) from lion_split) = (select count(*) from lion_split where k like 'k%'),
				'every row is still reachable through the index'"

	psql_p -c "SELECT injection_points_detach('lion-dir-split-incomplete')" \
		>>"$RUNLOG" 2>&1 || true
	psql_p -c "DROP TABLE lion_split" >>"$RUNLOG" 2>&1
	psql_p -c "DROP TABLE IF EXISTS lion_split_flush" >>"$RUNLOG" 2>&1

	log "phase 1c: the unfinished split survived the crash and a writer's descent repaired it ($before -> $after leaves)"
	SUMMARY+=("phase1c            crash between split and downlink; repaired on the next descent")
}


# ------------------------------------------------------------- phase 1d

# A crash that lands exactly between the two records of a POSTING-TREE LEAF
# SPLIT (DESIGN.md §22): the leaf has been split and flagged
# LION_PAGE_INCOMPLETE_SPLIT, its right sibling exists and is linked in, but
# the parent has no downlink for it yet, and the server dies.  Readers never
# noticed (the right link gets them there, and a sequential walk of the leaves
# is all they do), and the first writer that descends to that page finishes the
# split before touching it.
#
# The crash point is deterministic: an injection point parks the inserting
# backend in that window and the server is pulled out from under it.
phase1d() {
	local off before after warn parked
	log ""
	log "=== phase 1d: a crash between a posting-tree split and its downlink ==="

	psql_p -c "CREATE EXTENSION IF NOT EXISTS injection_points" >>"$RUNLOG" 2>&1 ||
		{ log "phase 1d skipped: this server has no injection_points extension"
		  SUMMARY+=("phase1d            skipped (no injection points)"); return 0; }

	# Two keys taking alternate TIDs, so each container key is a dense bitset
	# and fills a leaf by itself: the set is a tree after a few thousand rows.
	# The index is created EMPTY, so every split really happens in aminsert.
	psql_p >>"$RUNLOG" 2>&1 <<-SQL || die "phase 1d: fixture failed"
		SET synchronous_commit = on;
		DROP TABLE IF EXISTS lion_psplit;
		CREATE TABLE lion_psplit (id int, k int NOT NULL);
		CREATE INDEX lion_psplit_k ON lion_psplit USING lion (k);
		INSERT INTO lion_psplit SELECT i, i % 2 FROM generate_series(1, 60000) i;
	SQL
	psql_p -c "VACUUM (ANALYZE) lion_psplit" >>"$RUNLOG" 2>&1
	before=$(psql_p -tAc "select container_pages from lion_index_stats('lion_psplit_k')")
	[ "${before:-0}" -gt 2 ] ||
		die "phase 1d: the fixture did not split a posting page at all"

	psql_p -c "SELECT injection_points_attach('lion-posting-split-incomplete', 'wait')" \
		>>"$RUNLOG" 2>&1 || die "phase 1d: could not attach the injection point"

	off=$(stat -c %s "$PRIMARY_LOG")
	"$PGBIN/psql" -X -q -h "$SOCKDIR" -p "$PRIMARY_PORT" -U postgres -d "$DBNAME" \
		-c "SET synchronous_commit = on;
		    INSERT INTO lion_psplit SELECT 100000 + i, i % 2
		      FROM generate_series(1, 60000) i" >>"$RUNLOG" 2>&1 &
	parked=$!

	wait_true psql_p \
		"select count(*) > 0 from pg_stat_activity where wait_event = 'lion-posting-split-incomplete'" \
		60 "an insert to park between the posting split and its downlink"

	# The split's record has been inserted but not flushed, and the crash would
	# otherwise simply lose it (and with it the whole split, which would test
	# nothing).  A COMMIT from another session with synchronous_commit = on
	# flushes the WAL stream up to its own commit LSN, which is past the split
	# record.  A CHECKPOINT would be the obvious way and is NOT usable here:
	# the parked backend holds the leaf's content lock EXCLUSIVE and the
	# checkpointer would block writing that very buffer.
	psql_p >>"$RUNLOG" 2>&1 <<-SQL
		SET synchronous_commit = on;
		CREATE TABLE IF NOT EXISTS lion_psplit_flush (i int);
		INSERT INTO lion_psplit_flush VALUES (1);
	SQL

	crash_immediate
	wait "$parked" 2>/dev/null || true

	start_node "$PRIMARY_DATA" "$PRIMARY_PORT" "$PRIMARY_LOG"
	verify_node psql_p "$PRIMARY_DATA"

	tail -c "+$((off + 1))" "$PRIMARY_LOG" >>"$RUNLOG"
	tail -c "+$((off + 1))" "$PRIMARY_LOG" |
		grep -q "database system was not properly shut down" ||
		die "phase 1d: the restart did not report an unclean shutdown"

	# The page really came back flagged: verify() says so, as a WARNING and
	# not an error, and the index still answers correctly - a reader walks the
	# leaves by their right links and an unfinished split is invisible to it.
	warn=$(psql_p -c "SELECT lion_index_verify('lion_psplit_k')" 2>&1 >>"$RUNLOG" |
		grep -c 'unfinished split' || true)
	[ "$warn" -gt 0 ] ||
		die "phase 1d: no posting page came back with an unfinished split, so the crash missed the window"
	log "phase 1d: $warn posting page(s) came back with an unfinished split"
	run_check "phase 1d post-crash" psql_p \
		"select (select count(*) from lion_psplit where k = 0) =
				(select count(*) from lion_psplit where id % 2 = 0),
				'k = 0 still answers exactly'
		 union all
		 select (select count(*) from lion_psplit where k = 1) =
				(select count(*) from lion_psplit where id % 2 = 1),
				'k = 1 still answers exactly'"

	# A WRITER's descent is what repairs it - the descent that lands on the
	# flagged page finishes its split before touching the page - but only a
	# descent that ROUTES there does, and an append does not descend at all
	# (it takes the set's last leaf directly, which is the point of the append
	# hint).  So the rows below are made to land all over the heap instead:
	# a third of them is deleted, VACUUM frees those line pointers on every
	# page, and the inserts that refill them have container keys in every
	# leaf's range, including the flagged one's.
	psql_p >>"$RUNLOG" 2>&1 <<-SQL || die "phase 1d: the repairing insert failed"
		SET synchronous_commit = on;
		DELETE FROM lion_psplit WHERE id % 3 = 0;
		VACUUM lion_psplit;
		INSERT INTO lion_psplit SELECT 900000 + i, i % 2
		  FROM generate_series(1, 20000) i;
	SQL

	warn=$(psql_p -c "SELECT lion_index_verify('lion_psplit_k', true)" 2>&1 >>"$RUNLOG" |
		grep -c 'unfinished split' || true)
	[ "$warn" = 0 ] ||
		die "phase 1d: $warn posting page(s) still have an unfinished split after a writer descended"

	after=$(psql_p -tAc "select container_pages from lion_index_stats('lion_psplit_k')")
	run_check "phase 1d repaired" psql_p \
		"select (select count(*) from lion_psplit) =
				(select count(*) from lion_psplit where k in (0, 1)),
				'every row is still reachable through the index'
		 union all
		 select (select ntids from lion_index_stats('lion_psplit_k')) =
				(select count(*) from lion_psplit),
				'ntids agrees with the heap'"

	psql_p -c "SELECT injection_points_detach('lion-posting-split-incomplete')" \
		>>"$RUNLOG" 2>&1 || true
	psql_p -c "DROP TABLE lion_psplit" >>"$RUNLOG" 2>&1
	psql_p -c "DROP TABLE IF EXISTS lion_psplit_flush" >>"$RUNLOG" 2>&1

	log "phase 1d: the unfinished posting split survived the crash and a writer's descent repaired it ($before -> $after leaves)"
	SUMMARY+=("phase1d            crash between posting split and downlink; repaired on the next descent")
}

# ---------------------------------------------------------------- phase 2

basebackup_standby() {
	log "-- pg_basebackup -R into $STANDBY_DATA"
	"$PGBIN/pg_basebackup" -D "$STANDBY_DATA" -R -X stream -c fast --no-sync \
		-h "$SOCKDIR" -p "$PRIMARY_PORT" -U postgres >>"$RUNLOG" 2>&1 ||
		die "pg_basebackup failed (see $RUNLOG)"
	[ -f "$STANDBY_DATA/standby.signal" ] ||
		die "pg_basebackup -R did not write standby.signal"
	write_standby_conf on 2s
	echo "include 'standby_extra.conf'" >>"$STANDBY_DATA/postgresql.conf"
	start_node "$STANDBY_DATA" "$STANDBY_PORT" "$STANDBY_LOG"
	verify_node psql_s "$STANDBY_DATA"
	[ "$(psql_s -tAc 'select pg_is_in_recovery()')" = "t" ] ||
		die "the standby is not in recovery"
}

restart_standby() {
	"$PGBIN/pg_ctl" -D "$STANDBY_DATA" stop -m fast -w -t 60 >>"$RUNLOG" 2>&1 ||
		die "could not stop the standby"
	start_node "$STANDBY_DATA" "$STANDBY_PORT" "$STANDBY_LOG"
	verify_node psql_s "$STANDBY_DATA"
}

# The INSERT position, not pg_current_wal_lsn(), which is the WRITE position:
# a VACUUM assigns no transaction id and commits nothing, so its records can
# sit in the WAL buffers - unsent - behind a write pointer the standby has
# already passed.
wait_catchup() {
	wait_true psql_p \
		"select coalesce(bool_or(replay_lsn >= pg_current_wal_insert_lsn()), false) from pg_stat_replication" \
		120 "the standby to replay everything the primary has written"
}

# rr_variant <hot_standby_feedback on|off> <k4 value>
#
# DESIGN.md section 9: a standby count must never trust the visibility map, so
# a REPEATABLE READ snapshot that counted key K must keep seeing that number
# even after the primary deletes those rows and vacuums them away.  With
# feedback on the primary cannot remove them and the count must be unchanged;
# with feedback off it can, and the only other legal outcome is that the
# session is cancelled with a recovery conflict.
rr_variant() {
	local feedback=$1 key=$2 before after n1 n2 rc=0 out err fresh seqcnt
	log ""
	log "-- recovery conflict: hot_standby_feedback = $feedback, k4 = $key"
	write_standby_conf "$feedback" 2s
	restart_standby
	wait_catchup

	before=$(psql_p -tAc "select count(*) from lion_rec where k4 = $key")
	[ "$before" -gt 0 ] || die "k4 = $key has no rows on the primary; pick another key"

	out=$BASE/rr_$feedback.out
	err=$BASE/rr_$feedback.err
	"$PGBIN/psql" -X -q -At -v ON_ERROR_STOP=1 -h "$SOCKDIR" -p "$STANDBY_PORT" \
		-U postgres -d "$DBNAME" -v key="$key" -v hold=6 \
		-f "$HERE/standby_rr.sql" >"$out" 2>"$err" &
	RRPID=$!

	# Do not touch the primary until that session holds its snapshot and has
	# already counted the key, or the test would prove nothing.
	wait_true psql_s \
		"select count(*) > 0 from pg_stat_activity where pid <> pg_backend_pid() and backend_xmin is not null and state = 'active' and query ilike 'select pg_sleep%'" \
		30 "the standby REPEATABLE READ session to hold its snapshot"
	if [ "$feedback" = on ]; then
		wait_true psql_p \
			"select coalesce(bool_or(backend_xmin is not null), false) from pg_stat_replication" \
			30 "the standby's xmin to reach the primary (hot_standby_feedback = on)"
	else
		wait_true psql_p \
			"select coalesce(bool_and(backend_xmin is null), true) from pg_stat_replication" \
			30 "the primary to forget the standby's xmin (hot_standby_feedback = off)"
	fi

	psql_p -c "DELETE FROM lion_rec WHERE k4 = $key" >>"$RUNLOG" 2>&1
	psql_p -c "VACUUM (INDEX_CLEANUP ON) lion_rec" >>"$RUNLOG" 2>&1

	# Neither outcome below means anything unless the primary's VACUUM did
	# what this variant is about: with feedback on the standby's xmin must
	# have held the deleted TIDs in the index, with feedback off they must be
	# gone - that removal is what the recovery conflict exists to protect the
	# standby's snapshot from.
	local ntids live
	ntids=$(psql_p -tAc "select ntids from lion_index_stats('lion_rec_k4')")
	live=$(psql_p -tAc "select count(*) from lion_rec")
	if [ "$feedback" = on ]; then
		[ "$ntids" -gt "$live" ] ||
			die "hot_standby_feedback = on did not hold the primary's horizon back: the index has $ntids TIDs for $live live rows, so VACUUM removed the deleted ones and the test below proves nothing"
		log "   primary VACUUM was held back: index still has $ntids TIDs for $live live rows"
	else
		[ "$ntids" = "$live" ] ||
			die "hot_standby_feedback = off but the primary's VACUUM did not remove the deleted TIDs ($ntids in the index, $live live rows), so the conflict path was not reachable"
		log "   primary VACUUM removed the deleted TIDs: $ntids = $live"
	fi

	wait "$RRPID" || rc=$?
	RRPID=""
	n1=$(sed -n 's/^n1 //p' "$out" | head -1)
	n2=$(sed -n 's/^n2 //p' "$out" | head -1)
	{ echo "---- standby RR session (feedback=$feedback, exit $rc)"; cat "$out" "$err"; } >>"$RUNLOG"

	if [ "$rc" = 0 ]; then
		{ [ -n "$n1" ] && [ -n "$n2" ]; } ||
			die "the standby session exited 0 but did not print both counts (see $RUNLOG)"
		[ "$n1" = "$before" ] ||
			die "the standby's first count of k4 = $key was $n1, the primary had $before"
		[ "$n1" = "$n2" ] ||
			die "BUG: a standby REPEATABLE READ snapshot counted k4 = $key as $n1 and then as $n2 after the primary deleted and vacuumed those rows (hot_standby_feedback = $feedback)"
		log "   snapshot preserved: $n1 -> $n2, no conflict"
		SUMMARY+=("phase2 conflict feedback=$feedback  snapshot preserved, $n1 = $n2")
	else
		grep -qi "conflict with recovery" "$err" ||
			die "the standby session failed with something other than a recovery conflict (exit $rc): $(tail -3 "$err")"
		[ "$feedback" = off ] ||
			die "BUG: hot_standby_feedback is on, yet the standby session hit a recovery conflict: $(tail -2 "$err")"
		{ [ -z "$n2" ] || [ "$n2" = "$n1" ]; } ||
			die "BUG: the standby printed a second, different count ($n1 then $n2) before the conflict"
		log "   cancelled by a recovery conflict after counting $n1 (legal outcome)"
		SUMMARY+=("phase2 conflict feedback=$feedback  cancelled by a recovery conflict after counting $n1")
	fi

	# A fresh standby session, once caught up, must see the new count.
	wait_catchup
	after=$(psql_p -tAc "select count(*) from lion_rec where k4 = $key")
	fresh=$(psql_s -tAc "select lion_index_count('lion_rec_k4'::regclass, $key::int4)")
	seqcnt=$(psql_s -tAc "set enable_seqscan = on; set enable_bitmapscan = off; set enable_indexscan = off; set pg_lion.enable_count_pushdown = off; select count(*) from lion_rec where k4 = $key")
	[ "$fresh" = "$after" ] ||
		die "BUG: after catching up, a fresh standby session counts k4 = $key as $fresh; the primary says $after"
	[ "$seqcnt" = "$after" ] ||
		die "BUG: the standby's sequential scan counts k4 = $key as $seqcnt; the primary says $after"
	log "   fresh standby session sees the new count: $fresh (primary $after)"
	SUMMARY+=("phase2 catchup feedback=$feedback  fresh standby count of k4=$key is $fresh, primary $after")
}

# Chain free and page reuse, replayed on a standby while a reader works
# through the very pages being recycled (DESIGN.md §18).
#
# On a standby, generic WAL cannot raise a recovery conflict, so replay may
# reuse a page under a reader's feet.  What keeps that reader honest is the
# owner in each container page's special area: a page that no longer claims
# the chain a reader came from ends that reader's walk.  Here the primary
# empties two keys (entries deleted, chains freed, pages recorded free) and
# then fills two new ones, which take those pages back; a standby reader runs
# against the index throughout, and afterwards the standby must answer exactly
# what the primary answers for every key.
standby_chain_reuse() {
	local loop flag errf=$BASE/standby_reuse.err k p_out s_out freed
	log ""
	log "-- standby: chain free and page reuse under a reader"

	psql_p >>"$RUNLOG" 2>&1 <<-SQL || die "standby_chain_reuse: fixture failed"
		SET synchronous_commit = on;
		DROP TABLE IF EXISTS lion_reuse;
		CREATE TABLE lion_reuse (id int, k int NOT NULL);
		CREATE INDEX lion_reuse_k ON lion_reuse USING lion (k)
			WITH (inline_limit = 64);
		INSERT INTO lion_reuse SELECT i, i % 4 FROM generate_series(1, 40000) i;
	SQL
	psql_p -c "VACUUM (ANALYZE) lion_reuse" >>"$RUNLOG" 2>&1
	wait_catchup

	# A standby reader that keeps asking the index questions for as long as
	# the primary is freeing and recycling pages underneath it.  One short
	# query every 200 ms, not a tight loop: it has to coexist with replay,
	# not starve it.  The flag file is what stops it, so that nothing is left
	# spinning if a step below fails.
	flag=$BASE/standby_reuse.run
	: >"$flag"
	: >"$errf"
	(
		while [ -f "$flag" ]; do
			"$PGBIN/psql" -X -q -v ON_ERROR_STOP=1 -h "$SOCKDIR" \
				-p "$STANDBY_PORT" -U postgres -d "$DBNAME" -tAc \
				"SET enable_seqscan = off;
				 SELECT count(*) FROM lion_reuse WHERE k = 2;
				 SELECT count(*) FROM lion_reuse WHERE k = 3;
				 SELECT lion_index_count('lion_reuse_k'::regclass, 2::int4)" \
				>/dev/null 2>>"$errf" || true
			command sleep 0.2
		done
	) &
	loop=$!

	psql_p -c "SET synchronous_commit = on; DELETE FROM lion_reuse WHERE k IN (0, 1)" \
		>>"$RUNLOG" 2>&1 || { rm -f "$flag"; die "standby_chain_reuse: delete failed"; }
	# hot_standby_feedback is on here, so the primary's removal horizon is the
	# standby's oldest snapshot as the standby last REPORTED it, and that
	# report is only sent every wal_receiver_status_interval.  So the VACUUM
	# may find nothing removable for a few seconds after the delete; retry it
	# until the chains really are freed.
	wait_catchup
	freed=0
	for i in $(seq 1 40); do
		psql_p -c "VACUUM (INDEX_CLEANUP ON) lion_reuse" >>"$RUNLOG" 2>&1
		freed=$(psql_p -tAc "select deleted_pages from lion_index_stats('lion_reuse_k')")
		[ "${freed:-0}" -gt 0 ] && break
		nap 0.5
	done
	psql_p -c "SET synchronous_commit = on;
			   INSERT INTO lion_reuse SELECT i, 4 + (i % 2)
				 FROM generate_series(100001, 120000) i" >>"$RUNLOG" 2>&1 ||
		{ rm -f "$flag"; die "standby_chain_reuse: the reusing insert failed"; }
	psql_p -c "VACUUM (ANALYZE) lion_reuse" >>"$RUNLOG" 2>&1
	wait_catchup

	rm -f "$flag"
	wait "$loop" 2>/dev/null || true

	[ "${freed:-0}" -gt 0 ] ||
		die "standby_chain_reuse: the primary freed no pages (deleted_pages = $freed)"
	if grep -q . "$errf" 2>/dev/null; then
		head -10 "$errf" >&2
		die "standby_chain_reuse: the standby reader hit errors (see $errf)"
	fi

	# Every key, primary against standby, through the index on both sides.
	for k in 0 1 2 3 4 5; do
		p_out=$(psql_p -tAc "set enable_seqscan=off;
							 select count(*) from lion_reuse where k = $k")
		s_out=$(psql_s -tAc "set enable_seqscan=off;
							 select count(*) from lion_reuse where k = $k")
		[ "$p_out" = "$s_out" ] ||
			die "standby_chain_reuse: k = $k is $p_out on the primary and $s_out on the standby"
	done

	run_check "standby reuse" psql_s \
		"select (select count(*) from lion_reuse where k = 0) = 0,
				'freed key 0 answers no rows on the standby'
		 union all
		 select (select count(*) from lion_reuse where k = 4) = 10000,
				'recycled pages answer key 4 correctly on the standby'"

	psql_p -c "DROP TABLE lion_reuse" >>"$RUNLOG" 2>&1
	wait_catchup

	log "standby: $freed freed pages were recycled and replayed with a reader running"
	SUMMARY+=("phase2 standby      chain free + reuse replayed under a reader ($freed pages)")
}

phase2() {
	local probe_p probe_s ck
	log ""
	log "=== phase 2: hot standby ==="

	psql_p -c "VACUUM (ANALYZE, INDEX_CLEANUP ON) lion_rec" >>"$RUNLOG" 2>&1
	psql_p -c "CHECKPOINT" >>"$RUNLOG" 2>&1
	run_check "primary pre-basebackup" psql_p "select * from lion_rec_check(true, true)"
	ck=$NCHECKS
	run_check "primary count_stats" psql_p "select * from lion_rec_check_count_stats(false)"
	log "primary: $ck checks ok, and it does skip heap blocks via the visibility map"

	probe_p=$BASE/probe_primary.txt
	probe_s=$BASE/probe_standby.txt
	psql_p -tAc "select * from lion_rec_probe()" >"$probe_p"
	[ -s "$probe_p" ] || die "the primary probe produced nothing"

	basebackup_standby
	wait_catchup

	run_check "standby" psql_s "select * from lion_rec_check(true, false)"
	log "standby: $NCHECKS checks ok (verify, counts, GROUP BY, ntids)"
	SUMMARY+=("phase2 standby      $NCHECKS checks ok in recovery")

	if [ "$MODE" = rmgr ]; then
		# DESIGN.md §25: replay of an rmgr-mode removal takes the cleanup
		# lock the §9 interlock needs, so the standby may use the visibility
		# map again - and this is where that is proved to HAPPEN, not merely
		# to be allowed.
		run_check "standby count_stats (rmgr: VM trusted)" psql_s \
			"select * from lion_rec_check_count_stats(true, true)"
		log "standby: counts skip heap blocks via the visibility map (rmgr mode)"
		SUMMARY+=("phase2 standby      counts skip blocks via the VM (rmgr mode, §25)")
	else
		run_check "standby count_stats" psql_s "select * from lion_rec_check_count_stats(true)"
		log "standby: all $NCHECKS counts skipped 0 heap blocks and rechecked every TID"
		SUMMARY+=("phase2 standby      $NCHECKS counts skipped 0 blocks, rechecked every TID")
	fi

	psql_s -tAc "select * from lion_rec_probe()" >"$probe_s"
	if ! diff -u "$probe_p" "$probe_s" >/dev/null 2>&1; then
		diff -u "$probe_p" "$probe_s" | tee -a "$RUNLOG" | head -40 >&2
		die "BUG: the standby does not answer what the primary answers (see $RUNLOG)"
	fi
	log "standby: all $(wc -l <"$probe_s") index probes identical to the primary"
	SUMMARY+=("phase2 standby      $(wc -l <"$probe_s") index probes identical to the primary")

	standby_chain_reuse

	rr_variant on 11
	rr_variant off 7

	log ""
	log "-- promoting the standby"
	"$PGBIN/pg_ctl" -D "$STANDBY_DATA" promote -w -t 120 >>"$RUNLOG" 2>&1 ||
		die "pg_ctl promote failed"
	wait_true psql_s "select not pg_is_in_recovery()" 120 "the standby to finish promotion"
	run_check "promoted standby" psql_s "select * from lion_rec_check(true, false)"
	ck=$NCHECKS
	psql_s -c "VACUUM (INDEX_CLEANUP ON) lion_rec" >>"$RUNLOG" 2>&1
	run_check "promoted standby post-vacuum" psql_s "select * from lion_rec_check(false, true)"
	run_check "promoted standby count_stats" psql_s "select * from lion_rec_check_count_stats(false)"
	log "promoted standby: $ck checks ok, and it trusts the visibility map again"
	SUMMARY+=("phase2 promoted     $ck checks ok after promotion, VM path back in use")
}

# ---------------------------------------------------------------- phase 3

# The standby half of the §9/§11 interlock, attacked where it is thinnest
# (DESIGN.md §9 "Hot standby", §11 "On a standby", §25 "Standby").
#
# A standby reader copies containers out of an index page, keeps the PIN, and
# only then asks the visibility map about their heap blocks - exactly as on a
# primary.  In rmgr mode it trusts the map, on the argument that replay of
# every removal takes a CLEANUP lock.  Each case below parks such a reader at
# the injection point 'lion-count-containers-pinned' (containers copied, page
# pinned, map not yet consulted) and then has the PRIMARY move the TIDs the
# reader copied onto a page it is not pinning, and VACUUM them there:
#
#   split     an insert splits the leaf the reader pins; the containers with
#             the dead TIDs move to the new right sibling, and the VACUUM
#             removes them there.  The pinned leaf itself has nothing left to
#             remove, so the primary's VACUUM cleanup-locks it but writes no
#             record for it.
#   pushdown  the reader pins a one-page posting set's ROOT; an insert pushes
#             the root down (§22), so the containers move to a new child and
#             the root becomes an internal page.
#   spill     the reader pins a directory leaf holding an INLINE posting set;
#             the VACUUM's own filtering makes the payload outgrow the entry
#             and spill onto container pages, which rewrites the entry on the
#             pinned leaf.
#
# In every case the primary's VACUUM ends by marking the deleted rows' heap
# pages all-visible, and replay of those heap records follows replay of the
# index records.  So unless replay of the index side WAITS for the reader's
# pin, the reader - woken once the standby has caught up - counts the dead
# rows from its copy.  The legal outcomes are: replay blocks on the pin (the
# startup process shows up waiting for a cleanup lock - the barrier of §25,
# carried by a stand-alone VACUUM_VISIT record in the split case and riding on
# the removal record in the other two) and the reader, released
# then, answers the right number; or the standby rechecks every TID (generic
# mode) and answers the right number without blocking anything.  A standby
# that catches up while the reader is parked, and a reader that then answers
# anything else, is the bug.

# PHASE3_CASES (environment, a development aid) runs a subset of the cases.
want_case() { case " ${PHASE3_CASES:-split pushdown spill} " in *" $1 "*) return 0 ;; esac; return 1; }

# standby_pin_case <label> <index> <key> <expected> <sql to run on the primary>
standby_pin_case() {
	local label=$1 idx=$2 key=$3 expected=$4 sql=$5 out err rc=0 n i state="" rpid plsn
	out=$BASE/pin_$label.out
	err=$BASE/pin_$label.err

	"$PGBIN/psql" -X -q -At -v ON_ERROR_STOP=1 -h "$SOCKDIR" -p "$STANDBY_PORT" \
		-U postgres -d "$DBNAME" >"$out" 2>"$err" <<-SQL &
		SET pg_lion.enable_count_pushdown = off;
		SELECT injection_points_set_local();
		SELECT injection_points_attach('lion-count-containers-pinned', 'wait');
		SELECT 'count ' || lion_index_count('$idx'::regclass, $key::int4);
	SQL
	rpid=$!
	RRPID=$rpid
	wait_true psql_s \
		"select count(*) > 0 from pg_stat_activity where wait_event = 'lion-count-containers-pinned'" \
		30 "$label: the standby reader to park with its page pinned"

	# The trailing xid makes a commit record, whose synchronous flush takes the
	# VACUUM's own records (which commit nothing) to the standby with it.
	psql_p >>"$RUNLOG" 2>&1 <<-SQL || die "$label: the primary's writes failed"
		SET synchronous_commit = on;
		$sql
		SELECT pg_current_xact_id();
	SQL

	# Either replay blocks on the reader's pin, or it catches up.  Both are
	# asked of the standby itself: the primary's view of replay_lsn is only as
	# fresh as the last status message.
	plsn=$(psql_p -tAc "select pg_current_wal_insert_lsn()")
	for i in $(seq 1 300); do
		if [ "$(psql_s -tAc "select count(*) > 0 from pg_stat_activity where backend_type = 'startup' and wait_event in ('BufferPin', 'BufferCleanup')")" = t ]; then
			state=blocked
			break
		fi
		if [ "$(psql_s -tAc "select pg_last_wal_replay_lsn() >= '$plsn'::pg_lsn")" = t ]; then
			state=caughtup
			break
		fi
		nap 0.1
	done
	if [ -z "$state" ]; then
		psql_s -c "select backend_type, state, wait_event_type, wait_event, left(query, 60) from pg_stat_activity" >&2 || true
		die "$label: replay neither blocked nor caught up within 30s"
	fi

	psql_s -tAc "select injection_points_detach('lion-count-containers-pinned')" >/dev/null 2>&1 || true
	for i in $(seq 1 100); do
		n=$(psql_s -tAc "select count(*) from pg_stat_activity where wait_event = 'lion-count-containers-pinned'")
		[ "$n" = 0 ] && break
		psql_s -tAc "select injection_points_wakeup('lion-count-containers-pinned')" >/dev/null 2>&1 || true
		nap 0.1
	done
	wait "$rpid" || rc=$?
	RRPID=""
	{ echo "---- standby pin case $label (exit $rc, replay $state)"; cat "$out" "$err"; } >>"$RUNLOG"
	[ "$rc" = 0 ] || die "$label: the standby reader failed: $(tail -3 "$err")"
	n=$(sed -n 's/^count //p' "$out" | head -1)

	# A failure is recorded and the other cases still run, so that one run
	# says which of the three shapes is open.
	wait_catchup
	{ echo "---- $label: count_stats and ntids after catching up, standby then primary";
	  psql_s -tAc "select *, (select ntids from lion_index_stats('$idx')) from lion_index_count_stats('$idx'::regclass, $key::int4)";
	  psql_p -tAc "select *, (select ntids from lion_index_stats('$idx')) from lion_index_count_stats('$idx'::regclass, $key::int4)"; } >>"$RUNLOG" 2>&1
	if [ "$n" != "$expected" ]; then
		log "   BUG ($label): a standby count parked with its page pinned answered $n, the right answer for its snapshot is $expected (replay $state while it was parked, mode $MODE)"
		PHASE3_FAILS+=("$label")
	elif [ "$MODE" = rmgr ] && [ "$state" != blocked ]; then
		log "   BUG ($label): replay caught up past the primary's VACUUM while a standby reader held a pin on the page its containers came from (the answer happened to be right: $n)"
		PHASE3_FAILS+=("$label")
	else
		log "   $label: parked reader answered $n (expected $expected), replay $state while it was parked"
		SUMMARY+=("phase3 $label  standby reader $n = $expected, replay $state")
	fi
}

phase3() {
	local exp pages
	log ""
	log "=== phase 3: standby barriers for pinned readers (§9/§11/§25) ==="
	psql_p -tAc "select count(*) from pg_available_extensions where name = 'injection_points'" |
		grep -q '^1$' ||
		{ log "phase 3 skipped: this server has no injection_points extension"
		  SUMMARY+=("phase3             skipped (no injection points)"); return 0; }

	# A standby of its own: phase 2 promotes the one it made.
	stop_hard "$STANDBY_DATA"
	rm -rf "$STANDBY_DATA"
	psql_p >>"$RUNLOG" 2>&1 <<-SQL || die "phase 3: fixture failed"
		SET synchronous_commit = on;
		CREATE EXTENSION IF NOT EXISTS injection_points;

		-- split: five ~1.5 KB ARRAY containers per leaf (count_split_race.spec)
		DROP TABLE IF EXISTS pin_split;
		CREATE TABLE pin_split (id int, k int NOT NULL, pad char(300));
		INSERT INTO pin_split SELECT i, i % 2, '' FROM generate_series(1, 100000) i;
		CREATE INDEX pin_split_k ON pin_split USING lion (k);

		-- pushdown: one container on one page (count_root_pushdown_race.spec)
		DROP TABLE IF EXISTS pin_push;
		CREATE TABLE pin_push (id int, k int NOT NULL);
		INSERT INTO pin_push SELECT i, i % 4 FROM generate_series(1, 4000) i;
		CREATE INDEX pin_push_k ON pin_push USING lion (k) WITH (inline_limit = 64);

		-- spill: k = 1 is 200 consecutive offsets of heap block 0, one RUN
		-- container of a few bytes, INLINE; every other one deleted, it is a
		-- 100-member ARRAY that no longer fits inline_limit
		DROP TABLE IF EXISTS pin_spill;
		CREATE TABLE pin_spill (id int, k int NOT NULL);
		INSERT INTO pin_spill SELECT i, CASE WHEN i <= 200 THEN 1 ELSE 2 + i % 50 END
		  FROM generate_series(1, 3000) i;
		CREATE INDEX pin_spill_k ON pin_spill USING lion (k) WITH (inline_limit = 64);
	SQL
	psql_p -c "VACUUM (FREEZE, ANALYZE) pin_split" >>"$RUNLOG" 2>&1
	psql_p -c "VACUUM (FREEZE, ANALYZE) pin_push" >>"$RUNLOG" 2>&1
	psql_p -c "VACUUM (FREEZE, ANALYZE) pin_spill" >>"$RUNLOG" 2>&1
	# Free line pointers in heap blocks 0..63 only, so that the inserts of the
	# split case land there and grow the FIRST container of the first leaf.
	psql_p -c "SET synchronous_commit = on; DELETE FROM pin_split WHERE k = 0 AND (ctid::text::point)[0] < 64" >>"$RUNLOG" 2>&1
	# INDEX_CLEANUP ON: with fewer than 2% of the pages affected VACUUM would
	# otherwise bypass index vacuuming, leave the line pointers dead and never
	# tell the free space map about those pages.
	psql_p -c "VACUUM (INDEX_CLEANUP ON) pin_split" >>"$RUNLOG" 2>&1

	[ "$(psql_p -tAc "select max_posting_height from lion_index_stats('pin_push_k')")" = 0 ] ||
		die "phase 3: the pushdown fixture is not a one-page posting set"
	[ "$(psql_p -tAc "select inline_entries from lion_index_stats('pin_spill_k')")" = 1 ] ||
		die "phase 3: the spill fixture does not have exactly one INLINE entry"

	# Rows dead to everyone - deleted before the standby reader takes its
	# snapshot - and not vacuumed yet.
	psql_p >>"$RUNLOG" 2>&1 <<-SQL || die "phase 3: deletes failed"
		SET synchronous_commit = on;
		-- containers 1 and 2 of the first leaf (heap blocks 70..190)
		DELETE FROM pin_split WHERE k = 1 AND (ctid::text::point)[0] BETWEEN 70 AND 190;
		DELETE FROM pin_push WHERE k = 1 AND id <= 2000;
		DELETE FROM pin_spill WHERE k = 1 AND id % 2 = 0;
	SQL

	basebackup_standby
	write_standby_conf on -1
	restart_standby
	wait_catchup

	if want_case split; then
		exp=$(psql_p -tAc "select count(*) from pin_split where k = 1")
		pages=$(psql_p -tAc "select container_pages from lion_index_stats('pin_split_k')")
		# One range per stand-alone VACUUM_VISIT record, so that the barrier
		# this case depends on travels in that record type and not riding on
		# a removal record, which the other two cases cover.
		standby_pin_case split pin_split_k 1 "$exp" "
			INSERT INTO pin_split SELECT 1000000 + i, 1, '' FROM generate_series(1, 700) i;
			SET pg_lion.vacuum_barrier_ranges = 1;
			VACUUM (INDEX_CLEANUP ON) pin_split;
			RESET pg_lion.vacuum_barrier_ranges;"
		[ "$(psql_p -tAc "select container_pages from lion_index_stats('pin_split_k')")" -gt "$pages" ] ||
			die "phase 3: the split case did not split a leaf"
	fi

	if want_case pushdown; then
		exp=$(psql_p -tAc "select count(*) from pin_push where k = 1")
		standby_pin_case pushdown pin_push_k 1 "$exp" "
			INSERT INTO pin_push SELECT 100000 + i, CASE WHEN i % 2 = 0 THEN 1 ELSE 5 END
			  FROM generate_series(1, 40000) i;
			VACUUM (INDEX_CLEANUP ON) pin_push;"
		[ "$(psql_p -tAc "select max_posting_height from lion_index_stats('pin_push_k')")" -ge 1 ] ||
			die "phase 3: the pushdown case did not push the root down"
	fi

	if want_case spill; then
		exp=$(psql_p -tAc "select count(*) from pin_spill where k = 1")
		standby_pin_case spill pin_spill_k 1 "$exp" "
			VACUUM (INDEX_CLEANUP ON) pin_spill;"
		[ "$(psql_p -tAc "select inline_entries from lion_index_stats('pin_spill_k')")" = 0 ] ||
			die "phase 3: the spill case did not spill"
	fi

	run_check "phase 3 standby" psql_s "
		select lion_index_verify(i::regclass, true) is not null, 'verify ' || i
		  from unnest(array['pin_split_k', 'pin_push_k', 'pin_spill_k']) i
		union all
		select (select count(*) from pin_split where k = 1) = $(psql_p -tAc "select count(*) from pin_split where k = 1"), 'pin_split count'
		union all
		select (select count(*) from pin_push where k = 1) = $(psql_p -tAc "select count(*) from pin_push where k = 1"), 'pin_push count'
		union all
		select (select count(*) from pin_spill where k = 1) = $(psql_p -tAc "select count(*) from pin_spill where k = 1"), 'pin_spill count'"

	stop_hard "$STANDBY_DATA"
	psql_p -c "DROP TABLE pin_split, pin_push, pin_spill" >>"$RUNLOG" 2>&1
	[ "${#PHASE3_FAILS[@]}" = 0 ] ||
		die "phase 3: a standby reader was overtaken by replay in: ${PHASE3_FAILS[*]}"
}

# ---------------------------------------------------------------- main

mkdir -p "$LOGDIR"
: >"$RUNLOG"
rm -rf "$BASE"
mkdir -p "$BASE"
: >"$BASE/warnings.txt"
trap cleanup EXIT

START=$(now_ms)
log "pg_lion recovery tests"
log "prefix     $PREFIX"
log "server     $("$PGBIN/pg_config" --version)"
log "clusters   $PRIMARY_DATA (port $PRIMARY_PORT), $STANDBY_DATA (port $STANDBY_PORT)"
log "socket dir $SOCKDIR"
log ""

build_extension
init_primary

log "-- loading the fixture (test/recovery/schema.sql)"
psql_p -f "$HERE/schema.sql" >>"$RUNLOG" 2>&1 || die "schema.sql failed (see $RUNLOG)"
psql_p -c "VACUUM (ANALYZE, INDEX_CLEANUP ON) lion_rec" >>"$RUNLOG" 2>&1
# Without this the first crash replays everything since initdb, and the
# end-of-recovery checkpoint recycles the older half of that range before
# recovery_evidence() can read it.
psql_p -c "CHECKPOINT" >>"$RUNLOG" 2>&1
log "-- index shapes after the load and before any crash"
psql_p -tA -F'|' -c "select idx, ntids, entries, inline_entries, containers, sparse_segments, container_pages, leaf_pages, internal_pages, null_tids, empty_tids from lion_rec_indexes(), lion_index_stats(idx::regclass)" |
	tee -a "$RUNLOG"
run_check "baseline" psql_p "select * from lion_rec_check(true, true)"
log "-- baseline: $NCHECKS checks ok"

want_phase() { case " $PHASES " in *" $1 "*) return 0 ;; esac; return 1; }
want_phase 1 && phase1
want_phase 1b && phase1b
want_phase 1c && phase1c
want_phase 1d && phase1d
want_phase 2 && phase2
want_phase 3 && phase3

END=$(now_ms)
NWARN=$(wc -l <"$BASE/warnings.txt")
log ""
log "=== summary ==="
for s in "${SUMMARY[@]}"; do log "  $s"; done
log ""
log "  index WAL records replayed across all crashes ($MODE mode): $GENERIC_TOTAL"
log "  verify() warnings (unreferenced/empty pages, harmless by design): $NWARN"
log "  total time: $(( (END - START) / 1000 ))s"
log ""
log "ALL RECOVERY AND HOT-STANDBY CHECKS PASSED"
