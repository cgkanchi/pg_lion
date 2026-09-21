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
		-h|--help) sed -n '3,20p' "${BASH_SOURCE[0]}" | sed 's/^#\{0,1\} \{0,1\}//'; exit 0 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

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
	generic=$("$PGBIN/pg_waldump" -p "$PRIMARY_DATA/pg_wal" -r Generic \
		-s "$redo_start" -e "$redo_end" 2>/dev/null |
		grep -c 'rmgr: Generic' || true)
	# The write paths of this AM emit nothing but generic records, so a range
	# that replayed none of them replayed no index change at all.
	[ "$generic" -gt 0 ] ||
		die "the range $redo_start..$redo_end was replayed but holds no generic WAL record: this round tested no index change"
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
		log "iter $it: crash=$style vacuum_in_flight=$vacuum_crash redo_from=${ev% *} generic_wal_replayed=${ev#* } rows=$nrows checks=$((ck + NCHECKS)) $(( (t1 - t0) / 1000 ))s"
		SUMMARY+=("phase1 iter $it  crash=$style vacuum_in_flight=$vacuum_crash generic_wal_replayed=${ev#* } rows=$nrows checks=$((ck + NCHECKS))")
	done
	[ "$GENERIC_TOTAL" -gt 0 ] ||
		die "no generic WAL record was ever replayed: the crash phase proved nothing"
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

wait_catchup() {
	wait_true psql_p \
		"select coalesce(bool_or(replay_lsn >= pg_current_wal_lsn()), false) from pg_stat_replication" \
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

	run_check "standby count_stats" psql_s "select * from lion_rec_check_count_stats(true)"
	log "standby: all $NCHECKS counts skipped 0 heap blocks and rechecked every TID"
	SUMMARY+=("phase2 standby      $NCHECKS counts skipped 0 blocks, rechecked every TID")

	psql_s -tAc "select * from lion_rec_probe()" >"$probe_s"
	if ! diff -u "$probe_p" "$probe_s" >/dev/null 2>&1; then
		diff -u "$probe_p" "$probe_s" | tee -a "$RUNLOG" | head -40 >&2
		die "BUG: the standby does not answer what the primary answers (see $RUNLOG)"
	fi
	log "standby: all $(wc -l <"$probe_s") index probes identical to the primary"
	SUMMARY+=("phase2 standby      $(wc -l <"$probe_s") index probes identical to the primary")

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
psql_p -tA -F'|' -c "select idx, ntids, entries, inline_entries, containers, sparse_segments, container_pages, bucket_pages, null_tids, empty_tids from lion_rec_indexes(), lion_index_stats(idx::regclass)" |
	tee -a "$RUNLOG"
run_check "baseline" psql_p "select * from lion_rec_check(true, true)"
log "-- baseline: $NCHECKS checks ok"

phase1
phase2

END=$(now_ms)
NWARN=$(wc -l <"$BASE/warnings.txt")
log ""
log "=== summary ==="
for s in "${SUMMARY[@]}"; do log "  $s"; done
log ""
log "  generic WAL records replayed across all crashes: $GENERIC_TOTAL"
log "  verify() warnings (unreferenced/empty pages, harmless by design): $NWARN"
log "  total time: $(( (END - START) / 1000 ))s"
log ""
log "ALL RECOVERY AND HOT-STANDBY CHECKS PASSED"
