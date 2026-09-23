#!/bin/bash
#
# write_micro.sh - repeatable write-path measurements for the lion index.
#
# The numbers this produces are the ones the write wave is judged by:
#
#   build8      time and WAL of building the eight-index scalar portfolio
#               (the same eight columns bench/comprehensive uses) on 1M rows
#   ins10k      time and WAL of inserting 10,000 rows into that 1M-row table,
#               with one lion index and with eight, and per single index
#   burst       an 8-client pgbench insert burst (100 rows per transaction)
#               on a table with one lion index on a two-valued key
#   waits       wait-event sampling of the same burst (who is blocking whom)
#
# Everything runs on a PRIVATE cluster (not the dev cluster on 54329), so that
# the settings are the durable ones bench/comprehensive used - fsync on,
# full_page_writes on, wal_level replica, checkpoints out of the way - and so
# that a concurrent `make installcheck` on the dev cluster cannot perturb it.
# Write measurements are taken twice where it matters: with
# synchronous_commit off (the dev cluster's setting) and with it on.
#
# Usage:
#   bench/write_micro.sh init              # initdb + start + load the template
#   bench/write_micro.sh run LABEL         # everything -> results/LABEL.tsv
#   bench/write_micro.sh headline L [N]    # builds and inserts, N repeats plus
#                                          # medians (timings on this box drift
#                                          # 10-40% between sessions; WAL does
#                                          # not move at all, so compare that)
#   bench/write_micro.sh burst LABEL       # the 8-client bursts only
#   bench/write_micro.sh burstscale LABEL  # tps by clients x key count; env
#                                          # LION_WM_SCALE_KEYS, _SCALE_CLIENTS,
#                                          # _BURST_UNLOGGED=1 pick the cells
#   bench/write_micro.sh waits LABEL       # wait-event breakdown of the burst
#   bench/write_micro.sh wal LABEL [COLS]  # WAL composition per record type
#   bench/write_micro.sh {start|stop|psql|drop|reload}
#
# Environment:
#   LION_WM_PGBIN=<prefix>/bin   the PostgreSQL to measure with (default: the
#                                dev tree's assert build, which is fine for
#                                WAL bytes and wrong for milliseconds)
#   LION_WM_PRELOAD=1            start the cluster with
#                                shared_preload_libraries = 'pg_lion', so that
#                                wal_mode = auto resolves to rmgr (§25)
#
# The extension must be installed into .local/pg first:
#   make PG_CONFIG=.local/pg/bin/pg_config -s && make PG_CONFIG=... install
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# The PostgreSQL install to measure with.  The default is the dev tree's
# assert-enabled one, which is NOT what a headline number should come from;
# point LION_WM_PGBIN at a release prefix (bench/lib.sh's
# bench_build_extension installs the extension into one) for anything that
# goes into DESIGN.md.
PGBIN=${LION_WM_PGBIN:-$ROOT/.local/pg/bin}
DATA=${LION_WM_DATA:-$ROOT/.local/data-wm}
SOCK=${LION_WM_SOCK:-/tmp/claude-1000/pgsk-wm}
PORT=${LION_WM_PORT:-54330}
OUT=$ROOT/bench/results/write-micro
export PATH=$PGBIN:$PATH
export PGHOST=$SOCK PGPORT=$PORT PGUSER=postgres PGDATABASE=postgres

# The A/B lever of DESIGN.md §25: with LION_WM_PRELOAD=1 the private cluster
# starts with shared_preload_libraries = 'pg_lion', so every index built in it
# gets wal_mode = rmgr; without it they get generic WAL.  Nothing else differs,
# and the option is passed on pg_ctl's command line, so the data directory
# carries no trace of which arm last ran.
PRELOAD=${LION_WM_PRELOAD:-0}
PGOPTS=""
[ "$PRELOAD" = 1 ] && PGOPTS="-c shared_preload_libraries=pg_lion"

ROWS=${LION_WM_ROWS:-1000000}
NINS=${LION_WM_NINS:-10000}
BURST_SECONDS=${LION_WM_BURST_SECONDS:-5}
BURST_CLIENTS=${LION_WM_BURST_CLIENTS:-8}
# burstscale knobs: key counts, client counts, and whether the table (and so
# its index) is unlogged, which is how much of the critical section is WAL.
SCALE_KEYS=${LION_WM_SCALE_KEYS:-"2 1000"}
SCALE_CLIENTS=${LION_WM_SCALE_CLIENTS:-"1 4 8"}
BURST_UNLOGGED=${LION_WM_BURST_UNLOGGED:-0}
COLUMNS="c2 c20 c200 c20k c1m clustered skew nullable"

q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }

# ---------------------------------------------------------------- cluster ----

wm_init() {
	if [ -d "$DATA" ]; then echo "$DATA exists; run 'drop' first" >&2; exit 1; fi
	mkdir -p "$SOCK" "$OUT"
	initdb -D "$DATA" -U postgres --no-locale -E UTF8 >/dev/null
	cat >>"$DATA/postgresql.conf" <<CONF
port = $PORT
unix_socket_directories = '$SOCK'
listen_addresses = ''
shared_buffers = 512MB
work_mem = 64MB
maintenance_work_mem = 512MB
max_parallel_workers_per_gather = 0
max_parallel_maintenance_workers = 0
autovacuum = off
jit = off
# durable, and with checkpoints kept out of the measurements
fsync = on
synchronous_commit = off
full_page_writes = on
wal_level = replica
max_wal_size = 16GB
min_wal_size = 4GB
checkpoint_timeout = 1h
log_min_messages = warning
log_checkpoints = on
track_io_timing = on
CONF
	pg_ctl -D "$DATA" -l "$DATA/server.log" -o "$PGOPTS" -w start >/dev/null
	wm_load_template
	echo "write_micro cluster ready on $SOCK:$PORT"
}

# STOP and START, never `pg_ctl restart`, which reuses the previous
# postmaster's options out of postmaster.opts and would keep the preload of a
# previous rmgr arm (DESIGN.md §25 records the trap).
wm_start() { pg_ctl -D "$DATA" -l "$DATA/server.log" -o "$PGOPTS" -w start >/dev/null; }
wm_stop() { pg_ctl -D "$DATA" -m fast -w stop >/dev/null 2>&1 || true; }
wm_drop() { wm_stop; rm -rf "$DATA"; }

# The template database: the 1M-row fact table of bench/comprehensive's scalar
# suite plus the 10,000 rows every insert measurement adds to it.  Every
# measurement starts from a fresh copy of it, so the runs are independent.
wm_load_template() {
	psql -X -q -c "DROP DATABASE IF EXISTS wm_tpl" >/dev/null
	psql -X -q -c "CREATE DATABASE wm_tpl" >/dev/null
	q -d wm_tpl <<SQL
CREATE EXTENSION pg_lion;
CREATE TABLE fact AS $(scalar_data "$ROWS" 1);
CREATE TABLE ins$NINS AS $(scalar_data "$NINS" $((ROWS + 1)));
VACUUM (ANALYZE) fact;
CHECKPOINT;
SQL
	# A template must have no other sessions attached when it is copied.
	psql -X -q -c "ALTER DATABASE wm_tpl IS_TEMPLATE true" >/dev/null
}

# bench/comprehensive/workloads.py scalar_data(), verbatim in shape: eight
# columns of very different shapes over one 64-byte payload.
scalar_data() {
	local n=$1 start=$2 last=$((${2} + ${1} - 1))
	cat <<SQL
SELECT i::bigint AS id,
       ((hashint8extended(i::bigint,11) & 9223372036854775807)%2)::int AS c2,
       ((hashint8extended(i::bigint,22) & 9223372036854775807)%20)::int AS c20,
       ((hashint8extended(i::bigint,33) & 9223372036854775807)%200)::int AS c200,
       ((hashint8extended(i::bigint,44) & 9223372036854775807)%20000)::int AS c20k,
       ((hashint8extended(i::bigint,55) & 9223372036854775807)%1000000)::int AS c1m,
       LEAST(199, ((i-1)*200/$ROWS))::int AS clustered,
       CASE WHEN (hashint8extended(i::bigint,66) & 9223372036854775807)%10<9 THEN 0
            ELSE 1+((hashint8extended(i::bigint,77) & 9223372036854775807)%999)::int END AS skew,
       CASE WHEN (hashint8extended(i::bigint,88) & 9223372036854775807)%10=0 THEN NULL
            ELSE ((hashint8extended(i::bigint,99) & 9223372036854775807)%200)::int END AS nullable,
       repeat(md5(i::text),2) AS payload
  FROM generate_series($start::bigint,$last::bigint) i
SQL
}

fresh_db() {
	psql -X -q -c "DROP DATABASE IF EXISTS wm_run" >/dev/null
	psql -X -q -c "CREATE DATABASE wm_run TEMPLATE wm_tpl" >/dev/null
}

# ------------------------------------------------------------ measurements ---

# emit metric<TAB>ms<TAB>wal_bytes<TAB>extra
emit() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "${4:-}" | tee -a "$RESULT"; }

# Run one statement on wm_run, measured, and print "<ms> <wal_bytes>".
measure() {
	local sql=$1 syncmode=${2:-off}
	q -d wm_run -t -A -F' ' <<SQL
CHECKPOINT;
SET synchronous_commit = $syncmode;
SELECT pg_current_wal_insert_lsn() AS lsn0 \\gset
SELECT clock_timestamp() AS t0 \\gset
$sql
SELECT round(extract(epoch from clock_timestamp() - :'t0'::timestamptz) * 1000, 3),
       pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'lsn0'::pg_lsn)::bigint;
SQL
}

# The same statement twice, reporting both: the first run follows a checkpoint
# and therefore pays a full-page image for every page it touches for the first
# time, the second shows the steady-state cost.  Prints
# "<ms1> <wal1> <ms2> <wal2>".
measure_twice() {
	local sql=$1 syncmode=${2:-off}
	q -d wm_run -t -A -F' ' <<SQL
CHECKPOINT;
SET synchronous_commit = $syncmode;
SELECT pg_current_wal_insert_lsn() AS lsn0 \\gset
SELECT clock_timestamp() AS t0 \\gset
$sql
SELECT round(extract(epoch from clock_timestamp() - :'t0'::timestamptz) * 1000, 3) AS ms1,
       pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'lsn0'::pg_lsn)::bigint AS wal1,
       pg_current_wal_insert_lsn() AS lsn1, clock_timestamp() AS t1 \\gset
$sql
SELECT :'ms1', :'wal1',
       round(extract(epoch from clock_timestamp() - :'t1'::timestamptz) * 1000, 3),
       pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'lsn1'::pg_lsn)::bigint;
SQL
}
# The access method was renamed roaring -> lion.  The metric LABELS keep the old
# name on purpose, so that the numbers stay comparable with the files already in
# bench/results/write-micro; only the SQL is translated.
am_sql() { if [ "$1" = roaring ]; then echo lion; else echo "$1"; fi; }


build_sql() {
	local am=$1 cols=$2 out=""
	for c in $cols; do out="$out CREATE INDEX ix_$c ON fact USING $(am_sql "$am") ($c);"; done
	echo "$out"
}

measure_builds() {
	local am cols
	for am in roaring btree; do
		fresh_db
		set -- $(measure "$(build_sql $am "$COLUMNS")")
		emit "build8_$am" "$1" "$2" "8 indexes"
	done
	# one index at a time, roaring only: which column costs what
	for c in $COLUMNS; do
		fresh_db
		set -- $(measure "$(build_sql roaring "$c")")
		emit "build1_roaring_$c" "$1" "$2" ""
	done
}

measure_inserts() {
	local am cols label sync rest
	for sync in off on; do
		for spec in "roaring:$COLUMNS:8idx" "roaring:c200:1idx" "btree:$COLUMNS:8idx"; do
			am=${spec%%:*}; rest=${spec#*:}; cols=${rest%%:*}; label=${rest#*:}
			fresh_db
			q -d wm_run -c "$(build_sql $am "$cols")" >/dev/null
			set -- $(measure_twice "INSERT INTO fact SELECT * FROM ins$NINS;" "$sync")
			emit "ins${NINS}_${am}_${label}_sc_$sync" "$1" "$2" "after checkpoint"
			emit "ins${NINS}_${am}_${label}_sc_${sync}_steady" "$3" "$4" "second batch, no checkpoint"
		done
	done
	# per-column single-index inserts (roaring, synchronous_commit off)
	for c in $COLUMNS; do
		fresh_db
		q -d wm_run -c "$(build_sql roaring "$c")" >/dev/null
		set -- $(measure_twice "INSERT INTO fact SELECT * FROM ins$NINS;")
		emit "ins${NINS}_roaring_only_$c" "$1" "$2" "after checkpoint"
		emit "ins${NINS}_roaring_only_${c}_steady" "$3" "$4" "second batch"
	done
	fresh_db
	set -- $(measure_twice "INSERT INTO fact SELECT * FROM ins$NINS;")
	emit "ins${NINS}_noindex" "$1" "$2" "after checkpoint"
	emit "ins${NINS}_noindex_steady" "$3" "$4" "second batch"
}

# WAL composition of a 10,000-row insert with one lion index: which
# resource manager, how much of it full-page images, how much per record.
# Needs pg_walinspect, which is in contrib.
measure_wal_breakdown() {
	local col=${1:-c200} am=${2:-roaring}
	fresh_db
	q -d wm_run -c "CREATE EXTENSION IF NOT EXISTS pg_walinspect" >/dev/null
	q -d wm_run -c "$(build_sql $am "$col")" >/dev/null
	echo "--- WAL breakdown, $am on $col, second 10k-row batch (no checkpoint)" \
		| tee -a "$RESULT"
	q -d wm_run <<SQL | tee -a "$RESULT"
SET synchronous_commit = on;
CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS lsn0 \\gset
INSERT INTO fact SELECT * FROM ins$NINS;
SELECT pg_current_wal_flush_lsn() AS lsn1 \\gset
INSERT INTO fact SELECT * FROM ins$NINS;
SELECT pg_current_wal_flush_lsn() AS lsn2 \\gset
SELECT "resource_manager/record_type" AS record_type, count,
       round(record_size/1024.0/1024, 2) AS record_mib,
       round(fpi_size/1024.0/1024, 2) AS fpi_mib,
       round(combined_size::numeric / count, 1) AS bytes_per_record
  FROM pg_get_wal_stats(:'lsn1'::pg_lsn, :'lsn2'::pg_lsn, true)
 WHERE count > 0 ORDER BY combined_size DESC;
SQL
}

burst_setup() {
	local am=$1 sync=$2
	psql -X -q -c "DROP DATABASE IF EXISTS wm_burst" >/dev/null
	psql -X -q -c "CREATE DATABASE wm_burst" >/dev/null
	psql -X -q -c "ALTER DATABASE wm_burst SET synchronous_commit = $sync" >/dev/null
	q -d wm_burst <<SQL
CREATE EXTENSION pg_lion;
CREATE $([ "$BURST_UNLOGGED" = 1 ] && echo UNLOGGED) TABLE writes (k bigint, h int);
$([ "$am" = none ] || echo "CREATE INDEX ix_h ON writes USING $(am_sql "$am") (h);")
CHECKPOINT;
SQL
}

# 8-client insert burst, 100 rows per transaction on a two-valued key.
measure_burst() {
	local script=$OUT/.burst.sql logdir am sync tps p95 med wal
	cat >"$script" <<'SQL'
\set base random(1, 1000000000)
INSERT INTO writes SELECT (:base)::bigint * 1000 + i, i % 2 FROM generate_series(1, 100) i;
SQL
	for sync in on off; do
		for am in roaring btree none; do
			burst_setup "$am" "$sync"
			logdir=$(mktemp -d "$OUT/.pgbench-XXXXXX")
			local lsn0
			lsn0=$(psql -X -q -t -A -d wm_burst -c "SELECT pg_current_wal_insert_lsn()")
			( cd "$logdir" && pgbench -n -c "$BURST_CLIENTS" -j "$BURST_CLIENTS" \
				-T "$BURST_SECONDS" -M prepared -f "$script" --log \
				-d wm_burst >pgbench.out 2>&1 ) || { cat "$logdir/pgbench.out"; exit 1; }
			tps=$(awk '/^tps =/ {print $3; exit}' "$logdir/pgbench.out")
			# pgbench log: client_id transaction_no time(us) script_no ...
			read -r med p95 < <(awk '{print $3}' "$logdir"/pgbench_log.* | sort -n | awk '
				{a[NR]=$1} END {printf "%.3f %.3f\n", a[int(NR*0.5)+0]/1000, a[int(NR*0.95)+0]/1000}')
			wal=$(psql -X -q -t -A -d wm_burst \
				-c "SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), '$lsn0'::pg_lsn)::bigint")
			local rows
			rows=$(psql -X -q -t -A -d wm_burst -c "SELECT count(*) FROM writes")
			emit "burst${BURST_CLIENTS}_${am}_sc_$sync" "$med" "$wal" \
				"tps=$tps p95_ms=$p95 rows=$rows"
			rm -rf "$logdir"
		done
	done
}

# The headline numbers, repeated: this machine's run-to-run spread is 10-30%
# on timings (WAL is deterministic to the byte), so the report quotes medians
# over several repeats, taken with roaring and btree interleaved.
measure_headline() {
	local n=${1:-5} i
	for ((i = 1; i <= n; i++)); do
		fresh_db
		set -- $(measure "$(build_sql roaring "$COLUMNS")")
		emit "rep${i}_build8_roaring" "$1" "$2" ""
		fresh_db
		set -- $(measure "$(build_sql btree "$COLUMNS")")
		emit "rep${i}_build8_btree" "$1" "$2" ""

		fresh_db
		q -d wm_run -c "$(build_sql roaring "$COLUMNS")" >/dev/null
		set -- $(measure_twice "INSERT INTO fact SELECT * FROM ins$NINS;")
		emit "rep${i}_ins_roaring_8idx" "$1" "$2" "after checkpoint"
		emit "rep${i}_ins_roaring_8idx_steady" "$3" "$4" "second batch"

		fresh_db
		q -d wm_run -c "$(build_sql btree "$COLUMNS")" >/dev/null
		set -- $(measure_twice "INSERT INTO fact SELECT * FROM ins$NINS;")
		emit "rep${i}_ins_btree_8idx" "$1" "$2" "after checkpoint"
		emit "rep${i}_ins_btree_8idx_steady" "$3" "$4" "second batch"
	done
	echo "--- medians over $n repeats" | tee -a "$RESULT"
	awk -F'\t' '/^rep/ {split($1, a, "_"); k=substr($1, index($1, "_") + 1);
		ms[k] = ms[k] " " $2; wal[k] = wal[k] " " $3}
		END {for (k in ms) {
			n1 = split(ms[k], v, " "); asort_ms(v, n1); n2 = split(wal[k], w, " ");
			asort_ms(w, n2);
			printf "%-32s median %10.1f ms  %10.2f MiB\n", k, v[int((n1+1)/2)], w[int((n2+1)/2)]/1048576}}
		function asort_ms(arr, n,   i, j, t) {
			for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++)
				if (arr[j] + 0 < arr[i] + 0) {t = arr[i]; arr[i] = arr[j]; arr[j] = t}}' \
		"$RESULT" | sort | tee -a "$RESULT"
}

# Where does the contention of the burst live?  Three knobs, one at a time:
#   clients   1, 4, 8      - how far the workload scales at all
#   keys      2, 1000      - two keys mean two bucket pages and two container
#                            chains for every client; a thousand spread the
#                            index side over the whole directory while the heap
#                            contention stays exactly the same
#   am        roaring, btree, none
measure_burst_scale() {
	local script=$OUT/.burstk.sql am keys clients tps p95 med sync=on
	for keys in $SCALE_KEYS; do
		cat >"$script" <<SQL
\\set base random(1, 1000000000)
INSERT INTO writes SELECT (:base)::bigint * 1000 + i, i % $keys FROM generate_series(1, 100) i;
SQL
		for am in roaring btree none; do
			for clients in $SCALE_CLIENTS; do
				burst_setup "$am" "$sync"
				local logdir
				logdir=$(mktemp -d "$OUT/.pgbench-XXXXXX")
				( cd "$logdir" && pgbench -n -c "$clients" -j "$clients" \
					-T "$BURST_SECONDS" -M prepared -f "$script" --log \
					-d wm_burst >pgbench.out 2>&1 ) || { cat "$logdir/pgbench.out"; exit 1; }
				tps=$(awk '/^tps =/ {print $3; exit}' "$logdir/pgbench.out")
				read -r med p95 < <(awk '{print $3}' "$logdir"/pgbench_log.* | sort -n | awk '
					{a[NR]=$1} END {printf "%.3f %.3f\n", a[int(NR*0.5)+0]/1000, a[int(NR*0.95)+0]/1000}')
				emit "burst_${am}_keys${keys}_c${clients}$([ "$BURST_UNLOGGED" = 1 ] && echo _unlogged)" "$med" "-" \
					"tps=$tps p95_ms=$p95"
				rm -rf "$logdir"
			done
		done
	done
}

# Wait-event breakdown of the burst: what the eight clients are doing.
measure_waits() {
	local am sync=on script=$OUT/.burst.sql
	cat >"$script" <<'SQL'
\set base random(1, 1000000000)
INSERT INTO writes SELECT (:base)::bigint * 1000 + i, i % 2 FROM generate_series(1, 100) i;
SQL
	for am in roaring btree; do
		burst_setup "$am" "$sync"
		local logdir
		logdir=$(mktemp -d "$OUT/.pgbench-XXXXXX")
		( cd "$logdir" && pgbench -n -c "$BURST_CLIENTS" -j "$BURST_CLIENTS" \
			-T "$BURST_SECONDS" -M prepared -f "$script" -d wm_burst \
			>pgbench.out 2>&1 ) &
		local pgb=$!
		sleep 0.5
		echo "--- wait events, $am, ${BURST_CLIENTS} clients, synchronous_commit=$sync" \
			| tee -a "$RESULT"
		q -d wm_burst <<SQL | tee -a "$RESULT"
CREATE TEMP TABLE samples (wait_event_type text, wait_event text, n int);
DO \$\$
DECLARE deadline timestamptz := clock_timestamp() + interval '$((BURST_SECONDS - 2)) seconds';
BEGIN
	WHILE clock_timestamp() < deadline LOOP
		INSERT INTO samples
		SELECT wait_event_type, wait_event, count(*)
		  FROM pg_stat_activity
		 WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()
		   AND state = 'active'
		 GROUP BY 1, 2;
		PERFORM pg_sleep(0.004);
	END LOOP;
END \$\$;
SELECT coalesce(wait_event_type, 'CPU (running)') AS wait_event_type,
       coalesce(wait_event, '-') AS wait_event, sum(n) AS samples,
       round(100.0 * sum(n) / (SELECT sum(n) FROM samples), 1) AS pct
  FROM samples GROUP BY 1, 2 ORDER BY samples DESC;
SQL
		wait $pgb || true
		awk '/^tps =/ {print "tps = " $3}' "$logdir/pgbench.out" | tee -a "$RESULT"
		rm -rf "$logdir"
	done
}

# --------------------------------------------------------------------- main --

case "${1:-}" in
	init) wm_init ;;
	start) wm_start ;;
	stop) wm_stop ;;
	drop) wm_drop ;;
	reload) wm_load_template ;;
	psql) shift; exec psql -X "$@" ;;
	wal)
		LABEL=${2:?usage: write_micro.sh wal LABEL [COLUMN]}
		mkdir -p "$OUT"; RESULT=$OUT/$LABEL.wal.txt; : >"$RESULT"
		for c in ${3:-c2 c20 c200 c20k c1m}; do measure_wal_breakdown "$c" roaring; done
		measure_wal_breakdown c200 btree
		echo "-> $RESULT"
		;;
	headline)
		LABEL=${2:?usage: write_micro.sh headline LABEL [REPEATS]}
		mkdir -p "$OUT"; RESULT=$OUT/$LABEL.headline.tsv; : >"$RESULT"
		measure_headline "${3:-5}"
		echo "-> $RESULT"
		;;
	burstscale)
		LABEL=${2:?usage: write_micro.sh burstscale LABEL}
		mkdir -p "$OUT"; RESULT=$OUT/$LABEL.scale.tsv; : >"$RESULT"
		measure_burst_scale
		echo "-> $RESULT"
		;;
	burst)
		LABEL=${2:?usage: write_micro.sh burst LABEL}
		mkdir -p "$OUT"; RESULT=$OUT/$LABEL.tsv; touch "$RESULT"
		measure_burst
		echo "-> $RESULT"
		;;
	run)
		LABEL=${2:?usage: write_micro.sh run LABEL}
		mkdir -p "$OUT"; RESULT=$OUT/$LABEL.tsv; : >"$RESULT"
		echo "# $(date -u +%FT%TZ) $LABEL $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)" >>"$RESULT"
		printf '# metric\tms\twal_bytes\tnotes\n' >>"$RESULT"
		measure_builds
		measure_inserts
		measure_burst
		echo "-> $RESULT"
		;;
	waits)
		LABEL=${2:?usage: write_micro.sh waits LABEL}
		mkdir -p "$OUT"; RESULT=$OUT/$LABEL.waits.txt; : >"$RESULT"
		measure_waits
		echo "-> $RESULT"
		;;
	*) sed -n '3,30p' "$0"; exit 1 ;;
esac
