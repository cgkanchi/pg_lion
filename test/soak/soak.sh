#!/bin/bash
#
# The concurrent write + VACUUM + count soak (test/soak/README.md).
#
# Runs against a cluster that is ALREADY RUNNING, named by PGHOST/PGPORT
# (eval "$(./dev.sh env)"): it creates the database lion_soak there (dropping
# one left from an earlier run), loads the synthetic schema, and for
# --duration seconds runs, all at once:
#
#   writers   pgbench, alternating a MIXED phase (inserts, updates of indexed
#             and non-indexed columns, deletes, multi-statement transactions,
#             rollbacks, savepoints, upserts, racing speculative inserts)
#             with an ABORT phase (rate-limited writes that all roll back, so
#             the heap keeps coming back to all-visible under VACUUM);
#   vacuum    a loop of plain VACUUM, VACUUM (INDEX_CLEANUP ON) and
#             VACUUM (FREEZE, INDEX_CLEANUP ON), now and then in parallel and
#             with ANALYZE; back to back during an abort phase;
#   readers   loops that compare every lion-answered query with the same
#             query read by a sequential scan, inside one REPEATABLE READ or
#             SERIALIZABLE snapshot (soak.cmp() in checks.sql), some of them
#             holding the snapshot across VACUUMs (a long transaction, a
#             cursor paused part way);
#   verify    lion_index_verify(idx, heapallindexed => true) of every lion
#             index, and lion_index_stats() against the rows, periodically;
#   standby   with --standby-port: a streaming hot standby made with
#             pg_basebackup, and readers on it comparing the same way.
#
# Then it quiesces, VACUUMs, and checks everything once more on a still table
# (verify, exact ntids, a sweep of every query family) on the primary and the
# standby, and scans the server logs.  A mismatch, an ERROR other than a
# serialization failure, deadlock or recovery conflict, a verify WARNING, an
# assertion failure or a crash is a FAIL; the exit status is 1 if any.
#
# Options (defaults in brackets):
#   --duration S       seconds of concurrent load [600]
#   --rows N           rows of soak.t [300000]   --prows N  rows of soak.p [100000]
#   --writers N        pgbench clients [4]        --readers N  primary readers [2]
#   --mixed S          seconds of a mixed phase [75]
#   --abort S          seconds of an abort phase [45]
#   --abort-rate R     transactions per second of the abort phase [1]
#   --verify-every S   seconds between verify passes [150]
#   --out DIR          where the logs go [test/soak/out/<timestamp>]
#   --server-log F     the primary's server log [.local/pg.log]
#   --expect-mode M    generic | rmgr: fail unless the indexes are in mode M
#   --standby-port P   make a hot standby on port P (needs max_wal_senders > 0)
#   --standby-sock D   its socket directory [/tmp/lion-soak-standby-sock]
#   --standby-data D   its data directory [<out>/standby]
#   --standby-readers N  readers on it [2]
#   --run-as USER      run pg_basebackup and pg_ctl as USER (needed as root)
#   --seed N           pgbench's --random-seed and the readers' setseed [time]
#   --database NAME    [lion_soak]
#
# PG_CONFIG names the installation whose client programs are used
# [.local/pg/bin/pg_config].  ASAN_OPTIONS, if set, is passed through.
set -u

SOAK=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SOAK/../.." && pwd)
PG_CONFIG=${PG_CONFIG:-$ROOT/.local/pg/bin/pg_config}
PGBIN=$("$PG_CONFIG" --bindir) || { echo "soak: no pg_config at $PG_CONFIG" >&2; exit 2; }

DURATION=600 ROWS=300000 PROWS=100000 WRITERS=4 READERS=2
MIXED=75 ABORT=45 ABORT_RATE=1 VERIFY_EVERY=150
OUT="" SERVER_LOG=$ROOT/.local/pg.log EXPECT_MODE=""
SB_PORT="" SB_SOCK=/tmp/lion-soak-standby-sock SB_DATA="" SB_READERS=2 RUN_AS=""
SEED=$(date +%s) DB=lion_soak
while [ $# -gt 0 ]; do
	case $1 in
		--duration) DURATION=$2; shift ;;
		--rows) ROWS=$2; shift ;;
		--prows) PROWS=$2; shift ;;
		--writers) WRITERS=$2; shift ;;
		--readers) READERS=$2; shift ;;
		--mixed) MIXED=$2; shift ;;
		--abort) ABORT=$2; shift ;;
		--abort-rate) ABORT_RATE=$2; shift ;;
		--verify-every) VERIFY_EVERY=$2; shift ;;
		--out) OUT=$2; shift ;;
		--server-log) SERVER_LOG=$2; shift ;;
		--expect-mode) EXPECT_MODE=$2; shift ;;
		--standby-port) SB_PORT=$2; shift ;;
		--standby-sock) SB_SOCK=$2; shift ;;
		--standby-data) SB_DATA=$2; shift ;;
		--standby-readers) SB_READERS=$2; shift ;;
		--run-as) RUN_AS=$2; shift ;;
		--seed) SEED=$2; shift ;;
		--database) DB=$2; shift ;;
		-h|--help) sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//;$d'; exit 0 ;;
		*) echo "soak: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done

[ -n "${PGHOST:-}" ] && [ -n "${PGPORT:-}" ] ||
	{ echo "soak: set PGHOST and PGPORT (eval \"\$(./dev.sh env)\")" >&2; exit 2; }
export PGUSER=${PGUSER:-postgres}
[ -n "$OUT" ] || OUT=$SOAK/out/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd)
[ -n "$SB_DATA" ] || SB_DATA=$OUT/standby
PRIMARY_PGHOST=$PGHOST PRIMARY_PGPORT=$PGPORT
export PGDATABASE=$DB

LOG=$OUT/soak.log
FAILS=$OUT/failures.log
: >"$FAILS"
STOP=$OUT/.stop
PHASE=$OUT/.phase
rm -f "$STOP"
echo mixed >"$PHASE"

log() { echo "$(date '+%H:%M:%S') $*" | tee -a "$LOG"; }
psqlp() { "$PGBIN/psql" -X -q -v ON_ERROR_STOP=1 -h "$PRIMARY_PGHOST" -p "$PRIMARY_PGPORT" "$@"; }
psqls() { "$PGBIN/psql" -X -q -v ON_ERROR_STOP=1 -h "$SB_SOCK" -p "$SB_PORT" "$@"; }
as_server() {
	if [ -n "$RUN_AS" ]; then (cd / && runuser -u "$RUN_AS" -- env ${ASAN_OPTIONS:+ASAN_OPTIONS=$ASAN_OPTIONS} "$@")
	else "$@"; fi
}
# A FAIL: one line in failures.log, then the detail and the server log's tail.
fail() {
	local what=$1 detail=$2 slog=${3:-$SERVER_LOG}
	{
		echo "=== FAIL $(date '+%F %T') $what"
		echo "$detail"
		echo "--- tail of $slog"
		tail -n 40 "$slog" 2>/dev/null
		echo
	} >>"$FAILS"
	log "FAIL: $what"
}

# Everything this script started, stopped on any exit.
PIDS=()
cleanup() {
	touch "$STOP"
	for p in "${PIDS[@]}"; do pkill -P "$p" 2>/dev/null; kill "$p" 2>/dev/null; done
	wait 2>/dev/null
	if [ -n "$SB_PORT" ] && [ -f "$SB_DATA/postmaster.pid" ]; then
		as_server "$PGBIN/pg_ctl" -D "$SB_DATA" stop -m fast -w >/dev/null 2>&1
		psqlp -d postgres -c "SELECT pg_drop_replication_slot('lion_soak')" >/dev/null 2>&1
	fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ------------------------------------------------------------ preflight

SLOG_START=$(stat -c %s "$SERVER_LOG" 2>/dev/null || echo 0)
psqlp -d postgres -tAc 'select 1' >/dev/null || { echo "soak: cannot connect to $PGHOST:$PGPORT" >&2; exit 2; }
log "soak: primary $PRIMARY_PGHOST:$PRIMARY_PGPORT, $(psqlp -d postgres -tAc 'select version()')"
log "soak: duration ${DURATION}s, rows $ROWS + $PROWS, writers $WRITERS, readers $READERS, seed $SEED, out $OUT"
preload=$(psqlp -d postgres -tAc "show shared_preload_libraries")
sbuf=$(psqlp -d postgres -tAc "show shared_buffers")
log "soak: shared_preload_libraries = '$preload', shared_buffers = $sbuf"

psqlp -d postgres -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB" >/dev/null || exit 2
log "soak: loading the schema"
psqlp -v rows="$ROWS" -v prows="$PROWS" -f "$SOAK/schema.sql" >>"$LOG" 2>&1 || { echo "soak: schema.sql failed (see $LOG)" >&2; exit 2; }
psqlp -f "$SOAK/checks.sql" >>"$LOG" 2>&1 || { echo "soak: checks.sql failed (see $LOG)" >&2; exit 2; }
MODE=$(psqlp -tAc "SELECT string_agg(DISTINCT lion_index_wal_mode(idx), ',') FROM soak.lion_indexes")
log "soak: the indexes' WAL mode: $MODE"
if [ -n "$EXPECT_MODE" ] && [ "$MODE" != "$EXPECT_MODE" ]; then
	echo "soak: the indexes are in '$MODE' mode, not '$EXPECT_MODE'" >&2
	exit 2
fi

# ------------------------------------------------------------ standby

wait_catchup() {
	local lsn n=0
	lsn=$(psqlp -tAc "SELECT pg_current_wal_insert_lsn()")
	psqlp -c "SELECT pg_switch_wal()" >/dev/null 2>&1
	until [ "$(psqls -tAc "SELECT pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn" 2>/dev/null)" = t ]; do
		n=$((n + 1))
		[ $n -gt 1200 ] && { fail "standby catch-up" "the standby did not replay up to $lsn in 600s" "$SB_DATA/soak-standby.log"; return 1; }
		sleep 0.5
	done
}

if [ -n "$SB_PORT" ]; then
	senders=$(psqlp -tAc "show max_wal_senders")
	[ "$senders" -gt 0 ] || { echo "soak: --standby-port needs the primary started with max_wal_senders > 0" >&2; exit 2; }
	log "soak: making a hot standby on port $SB_PORT in $SB_DATA"
	as_server rm -rf "$SB_DATA"
	mkdir -p "$SB_SOCK" && chmod 700 "$SB_SOCK"
	[ -n "$RUN_AS" ] && chown "$RUN_AS" "$SB_SOCK" "$OUT"
	psqlp -d postgres -c "SELECT pg_drop_replication_slot('lion_soak')" >/dev/null 2>&1
	as_server "$PGBIN/pg_basebackup" -D "$SB_DATA" -R -X stream -C -S lion_soak -c fast --no-sync \
		-h "$PRIMARY_PGHOST" -p "$PRIMARY_PGPORT" -U postgres >>"$LOG" 2>&1 ||
		{ echo "soak: pg_basebackup failed (see $LOG)" >&2; exit 2; }
	as_server sh -c "cat >> '$SB_DATA/postgresql.conf'" <<-EOF
		# ---- test/soak/soak.sh standby
		port = $SB_PORT
		unix_socket_directories = '$SB_SOCK'
		listen_addresses = ''
		shared_buffers = 512MB
		hot_standby = on
		hot_standby_feedback = off
		max_wal_senders = $senders
		max_standby_streaming_delay = 600s
		max_standby_archive_delay = 600s
		wal_receiver_status_interval = 1s
		session_preload_libraries = 'pg_lion'
		shared_preload_libraries = '$preload'
		log_line_prefix = '%m [%p] '
	EOF
	# pg_basebackup copied whatever log the primary keeps in its data
	# directory; the standby's own goes to a file of its own name.
	as_server rm -f "$SB_DATA/soak-standby.log"
	as_server "$PGBIN/pg_ctl" -D "$SB_DATA" -l "$SB_DATA/soak-standby.log" -w -t 120 start >>"$LOG" 2>&1 ||
		{ echo "soak: the standby did not start (see $SB_DATA/soak-standby.log)" >&2; exit 2; }
	[ "$(psqls -tAc 'select pg_is_in_recovery()')" = t ] || { echo "soak: the standby is not in recovery" >&2; exit 2; }
	wait_catchup || exit 1
	log "soak: standby up and caught up ($(psqls -tAc "select count(*) from soak.t") rows in soak.t there)"
fi

# ------------------------------------------------------------ writers

WDIR=$OUT/writers
mkdir -p "$WDIR"
for f in "$SOAK"/writers/*.sql; do
	b=$(basename "$f" .sql)
	sed -e 's/@T@/soak.t/g' -e 's/:maxid/:tmax/g' -e 's/:loadmax/:tload/g' "$f" >"$WDIR/t_$b.sql"
	sed -e 's/@T@/soak.p/g' -e 's/:maxid/:pmax/g' -e 's/:loadmax/:pload/g' "$f" >"$WDIR/p_$b.sql"
done
# The mixed phase's weights: soak.t three times as busy as soak.p.
mix_args() {
	local w
	for s in ins:10 ins_multi:2 upd_idx:15 upd_some:5 upd_pad:10 del:30 txn:5 rollback:5 savepoint:5 upsert:5 spec:3; do
		w=${s#*:}
		echo "-f $WDIR/t_${s%:*}.sql@$((w * 3)) -f $WDIR/p_${s%:*}.sql@$w"
	done
}
abort_args() { echo "-f $WDIR/t_abort.sql@3 -f $WDIR/t_abort_sp.sql@1 -f $WDIR/p_abort.sql@1"; }

TLOAD=$(psqlp -tAc "SELECT tmax FROM soak.meta")
PLOAD=$(psqlp -tAc "SELECT pmax FROM soak.meta")
END=$(( $(date +%s) + DURATION ))
writers_loop() {
	local i=0 now left len tmax pmax
	while [ ! -f "$STOP" ]; do
		now=$(date +%s); left=$((END - now))
		[ $left -gt 0 ] || break
		tmax=$(psqlp -tAc "SELECT coalesce(max(id), 1) FROM soak.t WHERE id < 1000000000")
		pmax=$(psqlp -tAc "SELECT coalesce(max(id), 1) FROM soak.p WHERE id < 1000000000")
		if [ $((i % 2)) = 0 ]; then
			len=$MIXED; echo mixed >"$PHASE"
			[ $len -gt $left ] && len=$left
			echo "== $(date '+%T') mixed phase ${len}s tmax=$tmax pmax=$pmax" >>"$OUT/writers.log"
			"$PGBIN/pgbench" -n -c "$WRITERS" -j 2 -T "$len" --max-tries=20 --random-seed=$((SEED + i)) \
				-D tmax="$tmax" -D pmax="$pmax" -D tload="$TLOAD" -D pload="$PLOAD" $(mix_args) -h "$PRIMARY_PGHOST" -p "$PRIMARY_PGPORT" "$DB" \
				>>"$OUT/writers.log" 2>&1
		else
			len=$ABORT; echo abort >"$PHASE"
			[ $len -gt $left ] && len=$left
			echo "== $(date '+%T') abort phase ${len}s rate $ABORT_RATE" >>"$OUT/writers.log"
			"$PGBIN/pgbench" -n -c "$WRITERS" -j 2 -T "$len" -R "$ABORT_RATE" --random-seed=$((SEED + i)) \
				-D tmax="$tmax" -D pmax="$pmax" $(abort_args) -h "$PRIMARY_PGHOST" -p "$PRIMARY_PGPORT" "$DB" \
				>>"$OUT/writers.log" 2>&1
		fi
		echo "== pgbench exit $?" >>"$OUT/writers.log"
		i=$((i + 1))
	done
	echo done >"$PHASE"
}

# ------------------------------------------------------------ vacuum

vacuum_loop() {
	local i=0 cmd
	while [ ! -f "$STOP" ] && [ "$(date +%s)" -lt $END ]; do
		i=$((i + 1))
		if [ "$(cat "$PHASE")" = abort ]; then
			cmd="VACUUM (INDEX_CLEANUP ON) soak.t"
			[ $((i % 4)) = 0 ] && cmd="VACUUM (INDEX_CLEANUP ON) soak.t, soak.p"
		elif [ $((i % 7)) = 0 ]; then
			cmd="VACUUM (FREEZE, INDEX_CLEANUP ON) soak.t, soak.p"
		elif [ $((i % 5)) = 0 ]; then
			cmd="VACUUM (PARALLEL 2, INDEX_CLEANUP ON) soak.t"
		elif [ $((i % 3)) = 0 ]; then
			cmd="VACUUM (INDEX_CLEANUP ON) soak.t, soak.p"
		elif [ $((i % 11)) = 0 ]; then
			cmd="VACUUM ANALYZE soak.t, soak.p"
		else
			cmd="VACUUM soak.t, soak.p"
		fi
		echo "$(date '+%T') $cmd" >>"$OUT/vacuum.log"
		PGOPTIONS="-c max_parallel_maintenance_workers=2" psqlp -c "$cmd" >>"$OUT/vacuum.log" 2>&1 ||
			echo "$(date '+%T') ^ exit $?" >>"$OUT/vacuum.log"
		if [ "$(cat "$PHASE")" = abort ]; then sleep 0.3; else sleep 2; fi
	done
}

# ------------------------------------------------------------ verify

verify_pass() {
	local where=$1 node=$2 out idx
	out=$OUT/verify-$where.log
	echo "== $(date '+%T') verify pass" >>"$out"
	for idx in $($node -tAc "SELECT idx FROM soak.lion_indexes ORDER BY 1"); do
		$node -c "SELECT lion_index_verify('$idx', true)" >>"$out" 2>&1 ||
			echo "verify-error $idx" >>"$out"
	done
	$node -At -F'|' -c "BEGIN ISOLATION LEVEL REPEATABLE READ" \
		-c "SELECT * FROM soak.stats_check(false)" -c "COMMIT" >>"$OUT/reader-$where-stats.out" 2>&1
}
verify_loop() {
	local next=$(( $(date +%s) + VERIFY_EVERY ))
	while [ ! -f "$STOP" ] && [ "$(date +%s)" -lt $END ]; do
		if [ "$(date +%s)" -ge $next ]; then
			verify_pass primary psqlp
			[ -n "$SB_PORT" ] && verify_pass standby psqls
			next=$(( $(date +%s) + VERIFY_EVERY ))
		fi
		sleep 2
	done
}

# ------------------------------------------------------------ readers

# One reader: rounds of compared queries, in a snapshot of a random kind.
reader_loop() {
	local name=$1 node=$2 n=0 r iso sql
	local out=$OUT/reader-$name.out err=$OUT/reader-$name.err
	while [ ! -f "$STOP" ] && [ "$(date +%s)" -lt $END ]; do
		n=$((n + 1))
		r=$((RANDOM % 100))
		iso="REPEATABLE READ"
		# (a hot standby refuses SERIALIZABLE)
		[ "$name" = "${name#s}" ] && [ $((RANDOM % 4)) = 0 ] && iso=SERIALIZABLE
		if [ "$(cat "$PHASE")" = abort ] && [ $((RANDOM % 2)) = 0 ]; then
			# The abort phase's target: the aggregates over keys on a heap
			# that VACUUM keeps marking all-visible again.
			sql="SELECT * FROM soak.round('soak.t', 6, 'keyaggs');"
		elif [ $r -lt 65 ]; then
			sql="SELECT * FROM soak.round('soak.t', 8); SELECT * FROM soak.round('soak.p', 4);"
		elif [ $r -lt 80 ]; then
			# a long transaction: the same snapshot before and after VACUUMs
			sql="SELECT * FROM soak.round('soak.t', 4); SELECT pg_sleep(8 + random() * 10) \\g /dev/null
				 SELECT * FROM soak.round('soak.t', 6); SELECT * FROM soak.round('soak.p', 3);"
		else
			# a cursor paused part way through its node
			sql="SELECT * FROM soak.cursor_check('soak.t', 3 + random() * 6); SELECT * FROM soak.cursor_check('soak.p', 2);"
		fi
		{
			echo "SELECT setseed(($SEED + $n * 7919 + $RANDOM) % 1000000 / 1000000.0) \\g /dev/null"
			echo "BEGIN ISOLATION LEVEL $iso;"
			echo "$sql"
			echo "COMMIT;"
		} | $node -At -F'|' -v ON_ERROR_STOP=0 >>"$out" 2>>"$err"
	done
}

log "soak: starting the load"
writers_loop & PIDS+=($!)
vacuum_loop & PIDS+=($!)
verify_loop & PIDS+=($!)
for i in $(seq 1 "$READERS"); do reader_loop "p$i" psqlp & PIDS+=($!); done
if [ -n "$SB_PORT" ]; then
	for i in $(seq 1 "$SB_READERS"); do reader_loop "s$i" psqls & PIDS+=($!); done
fi

while [ "$(date +%s)" -lt $END ]; do
	sleep 10
	if ! psqlp -d postgres -tAc 'select 1' >/dev/null 2>&1; then
		fail "primary unreachable" "the primary stopped answering mid-run"
		break
	fi
	log "soak: $(( END - $(date +%s) ))s left, phase $(cat "$PHASE"), checks so far: $(cat "$OUT"/reader-*.out 2>/dev/null | grep -c '^ok|')"
done
touch "$STOP"
log "soak: stopping the load"
for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done
PIDS=()

# ------------------------------------------------------------ final checks

log "soak: final VACUUM and checks on a still table"
psqlp -c "VACUUM (FREEZE, INDEX_CLEANUP ON, ANALYZE) soak.t, soak.p" >>"$LOG" 2>&1 ||
	fail "final VACUUM" "$(tail -5 "$LOG")"
final_checks() {
	local where=$1 node=$2
	verify_pass "$where" "$node"
	{
		echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"
		echo "SELECT * FROM soak.sweep('soak.t', 6);"
		echo "SELECT * FROM soak.sweep('soak.p', 3);"
		echo "SELECT * FROM soak.cursor_check('soak.t', 0);"
		echo "SELECT * FROM soak.stats_check(true);"
		echo "COMMIT;"
	} | $node -At -F'|' -v ON_ERROR_STOP=0 >>"$OUT/reader-$where-final.out" 2>>"$OUT/reader-$where-final.err"
}
final_checks primary psqlp
if [ -n "$SB_PORT" ]; then
	wait_catchup && final_checks standby psqls
fi

# ------------------------------------------------------------ verdict

log "soak: collecting the results"
for f in "$OUT"/reader-*.out; do
	grep -E '^(FAIL|ERROR)\|' "$f" | while IFS= read -r line; do
		case $f in *-s[0-9]*|*standby*) sl=$SB_DATA/soak-standby.log ;; *) sl=$SERVER_LOG ;; esac
		fail "$(basename "$f" .out): $(echo "$line" | cut -d'|' -f1-3)" "$line" "$sl"
	done
done
# Errors the readers' psql saw outside soak.cmp(): a COMMIT's serialization
# failure, a recovery conflict and a cancelled statement are expected; a lost
# connection is a crash.
for f in "$OUT"/reader-*.err; do
	[ -s "$f" ] || continue
	grep -E 'ERROR|FATAL|PANIC|server closed|terminated|connection' "$f" |
		grep -vE 'could not serialize|conflict with recovery|deadlock detected|canceling statement due to (conflict|user request)|User was holding|User query might have needed' |
		sort | uniq -c | while IFS= read -r line; do
			fail "$(basename "$f"): psql error" "$line"
		done
done
# Writers: a pgbench client that stopped on an error other than the ones it
# retries.
grep -E 'aborted in command|ERROR' "$OUT/writers.log" 2>/dev/null |
	grep -vE 'could not serialize|deadlock detected' | sort | uniq -c | head -20 | while IFS= read -r line; do
		fail "writers: pgbench error" "$line"
	done
# Verify: any WARNING or ERROR.
for f in "$OUT"/verify-*.log; do
	[ -f "$f" ] || continue
	grep -E 'WARNING|ERROR|verify-error' "$f" | sort | uniq -c | while IFS= read -r line; do
		fail "$(basename "$f"): $line" "$line"
	done
done
# VACUUM: any error.
grep -E 'ERROR|WARNING' "$OUT/vacuum.log" 2>/dev/null | sort | uniq -c | while IFS= read -r line; do
	fail "vacuum: $line" "$line"
done
# The server logs since the run began.
scan_log() {
	local f=$1 from=$2 what=$3
	[ -f "$f" ] || return
	tail -c +"$((from + 1))" "$f" >"$OUT/$what-server.log"
	grep -nE 'TRAP:|PANIC|terminated by signal|was terminated|exited with exit code|ERROR: +[^ ]*(sanitizer|AddressSanitizer)|runtime error:|AddressSanitizer|LeakSanitizer|WARNING' "$OUT/$what-server.log" |
		grep -vE 'could not serialize|deadlock|conflict with recovery|canceling|"logical replication launcher" \(PID [0-9]+\) exited with exit code 1' |
		head -50 >"$OUT/$what-server.anomalies"
	if [ -s "$OUT/$what-server.anomalies" ]; then
		fail "$what server log: $(head -1 "$OUT/$what-server.anomalies" | cut -c1-200)" "$(cat "$OUT/$what-server.anomalies")" "$OUT/$what-server.log"
	fi
	grep -E 'ERROR:' "$OUT/$what-server.log" | sed -E 's/^[^E]*ERROR: +//' | cut -c1-120 | sort | uniq -c | sort -rn >"$OUT/$what-server.errors"
}
scan_log "$SERVER_LOG" "$SLOG_START" primary
[ -n "$SB_PORT" ] && scan_log "$SB_DATA/soak-standby.log" 0 standby

{
	echo "== soak summary ($(date '+%F %T'))"
	echo "mode: $MODE   preload: '$preload'   shared_buffers: $sbuf   duration: ${DURATION}s   seed: $SEED   standby: ${SB_PORT:-none}"
	echo "rows now: soak.t $(psqlp -tAc 'select count(*) from soak.t'), soak.p $(psqlp -tAc 'select count(*) from soak.p')"
	echo "writer transactions: $(grep -h 'number of transactions actually processed' "$OUT/writers.log" | awk '{s += $NF} END {print s + 0}')" \
		"(failed: $(grep -h 'number of failed transactions' "$OUT/writers.log" | awk '{s += $5} END {print s + 0}'))"
	echo "vacuums: $(grep -c VACUUM "$OUT/vacuum.log")   verify passes: $(cat "$OUT"/verify-*.log 2>/dev/null | grep -c '== ')"
	# narrow_containers is a column only builds with NARROW containers have
	echo "containers at the end (table: array bitset run narrow):"
	psqlp -tA -F' ' -c "SELECT i.tbl, sum(s.array_containers), sum(s.bitset_containers), sum(s.run_containers),
			coalesce(sum((to_jsonb(s) ->> 'narrow_containers')::int8)::text, '-')
		  FROM soak.lion_indexes i, lion_index_stats(i.idx) s GROUP BY i.tbl ORDER BY 1" 2>&1 | sed 's/^/  /'
	echo "checks by status:"
	cat "$OUT"/reader-*.out 2>/dev/null | grep -E '^(ok|FAIL|ERROR|expected)\|' | cut -d'|' -f1 | sort | uniq -c
	echo "checks by family and path (what answered the lion side):"
	cat "$OUT"/reader-*.out 2>/dev/null | grep -E '^(ok|FAIL|ERROR)\|' | cut -d'|' -f2,3 | sort | uniq -c |
		awk '{printf "  %-22s %7d\n", $2, $1}'
	echo "expected errors: $(cat "$OUT"/reader-*.out 2>/dev/null | grep -c '^expected|')"
	echo "server log ERRORs by message (primary):"
	head -15 "$OUT/primary-server.errors" 2>/dev/null
	echo "FAILs: $(grep -c '^=== FAIL' "$FAILS")"
	grep '^=== FAIL' "$FAILS" | head -30
} | tee "$OUT/summary.txt" | tee -a "$LOG"

[ "$(grep -c '^=== FAIL' "$FAILS")" = 0 ]
