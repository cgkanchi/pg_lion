#!/bin/bash
#
# The soak (soak.sh) in each WAL mode against the dev cluster in .local/data,
# which is restarted into each mode the way test/rmgr-check.sh does it - with
# the options on pg_ctl's command line, never in postgresql.conf - and put
# back afterwards: running in generic mode if it was running, else stopped.
#
#   make soak                                   both modes, 10 minutes each,
#                                               a hot standby in rmgr mode
#   SOAK_DURATION=120 SOAK_MODES=rmgr make soak
#
# Environment:
#   SOAK_MODES          the modes to run, in order ["generic rmgr"]
#   SOAK_STANDBY_MODES  the modes that also get a hot standby ["rmgr"]
#   SOAK_DURATION       seconds of load per mode [600]
#   SOAK_ARGS           more soak.sh options ("--rows 100000 --readers 1")
#   SOAK_RUN_AS         the user the cluster runs as, when this runs as root
#   SOAK_SERVER_OPTS    more server options for the restart, e.g.
#                       "-c shared_buffers=16MB" (a pool small enough that
#                       long lists run past the pin budget: DESIGN.md §15)
#   SOAK_STANDBY_PORT   [port + 1]   SOAK_STANDBY_SOCK  [<socket dir>-standby]
#   SOAK_STANDBY_DATA   [.local/soak-standby]
#   LION_SOCK, LION_PORT  as for dev.sh
#   PG_CONFIG           [.local/pg/bin/pg_config]
#   ASAN_OPTIONS        passed to the server and the client programs
# Each mode's results go to test/soak/out/<timestamp>-<mode>/; the exit
# status is 1 if any mode FAILed.
set -u
cd "$(dirname "$0")/../.." || exit 2
ROOT=$PWD
PGDATA=$ROOT/.local/data
PG_CONFIG=${PG_CONFIG:-$ROOT/.local/pg/bin/pg_config}
BINDIR=$("$PG_CONFIG" --bindir) || exit 2
LOG=$ROOT/.local/pg.log
MODES=${SOAK_MODES:-generic rmgr}
SB_MODES=${SOAK_STANDBY_MODES:-rmgr}
RUN_AS=${SOAK_RUN_AS:-}
eval "$("$ROOT/dev.sh" env)"
SB_PORT=${SOAK_STANDBY_PORT:-$((PGPORT + 1))}
SB_SOCK=${SOAK_STANDBY_SOCK:-$PGHOST-standby}
SB_DATA=${SOAK_STANDBY_DATA:-$ROOT/.local/soak-standby}

if [ "$(id -u)" = 0 ] && [ -z "$RUN_AS" ]; then
	echo "modes.sh: as root, set SOAK_RUN_AS=<the user the dev cluster runs as>" >&2
	exit 2
fi
as_server() {
	if [ -n "$RUN_AS" ]; then (cd / && runuser -u "$RUN_AS" -- env ${ASAN_OPTIONS:+ASAN_OPTIONS=$ASAN_OPTIONS} "$@")
	else "$@"; fi
}

if as_server "$BINDIR/pg_ctl" -D "$PGDATA" status >/dev/null 2>&1; then WAS_RUNNING=1; else WAS_RUNNING=0; fi
restore() {
	as_server "$BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null 2>&1
	if [ "$WAS_RUNNING" = 1 ]; then
		echo "== modes.sh: restoring the dev cluster to generic mode"
		as_server "$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" start >/dev/null 2>&1
	fi
}
trap restore EXIT

rc=0
stamp=$(date +%Y%m%d-%H%M%S)
for mode in $MODES; do
	opts="${SOAK_SERVER_OPTS:-}"
	standby=()
	[ "$mode" = rmgr ] && opts="$opts -c shared_preload_libraries=pg_lion"
	if [[ " $SB_MODES " == *" $mode "* ]]; then
		opts="$opts -c max_wal_senders=4"
		standby=(--standby-port "$SB_PORT" --standby-sock "$SB_SOCK" --standby-data "$SB_DATA")
		[ -n "$RUN_AS" ] && standby+=(--run-as "$RUN_AS")
	fi
	echo "== modes.sh: restarting the dev cluster for $mode mode: ${opts:-(no options)}"
	as_server "$BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null 2>&1
	if [ -n "$opts" ]; then
		as_server "$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" -o "$opts" -w start >/dev/null || exit 2
	else
		as_server "$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" -w start >/dev/null || exit 2
	fi
	registered=$("$BINDIR/psql" -X -tAc "SELECT count(*) FROM pg_get_wal_resource_managers() WHERE rm_name = 'pg_lion'")
	if { [ "$mode" = rmgr ] && [ "$registered" != 1 ]; } || { [ "$mode" = generic ] && [ "$registered" != 0 ]; }; then
		echo "== modes.sh: the server's resource managers do not match $mode mode" >&2
		exit 2
	fi
	# shellcheck disable=SC2086
	"$ROOT/test/soak/soak.sh" --duration "${SOAK_DURATION:-600}" --expect-mode "$mode" \
		--server-log "$LOG" --out "$ROOT/test/soak/out/$stamp-$mode" "${standby[@]}" ${SOAK_ARGS:-} || rc=1
done
exit $rc
