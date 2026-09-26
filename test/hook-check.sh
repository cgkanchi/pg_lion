#!/bin/bash
#
# Hook coexistence (DESIGN.md §23, "hook coexistence"): pg_lion next to
# another extension that uses the same planner hook and its own WAL resource
# manager, in both load orders; and the table-AM refusals of DESIGN.md §2,
# which need a table AM other than the heap.  The other extension is
# test/modules/lion_hooktest, which provides that table AM as well, and which
# this script builds and installs into the same installation.
#
#   make hookcheck                    the checks below
#   make hookcheck HOOKCHECK_FULL=1   ... and the whole regression suite in
#                                         both preload orders
#
# Against the dev cluster, like test/rmgr-check.sh, and for the same reason
# restarted with the options on pg_ctl's command line rather than in
# postgresql.conf: an interrupted run leaves no trace.  On exit the cluster is
# put back the way it was found: restarted in generic mode if it was running,
# left stopped if it was stopped.
#
#   1. nothing preloaded: the table-AM test, then LOAD-on-first-use in both
#      orders - lion_hooktest LOADed before pg_lion's first use, and after;
#   2. shared_preload_libraries = 'pg_lion,lion_hooktest' (the other hook
#      outermost) and 'lion_hooktest,pg_lion' (pg_lion's outermost), with
#      pg_lion.rmgr_id moved to 129 so the two resource managers coexist;
#   3. both resource managers on the default id 128: the server must refuse
#      to start, and say why.
#
# Each order-sensitive test checks from inside the other hook which one ran
# first, that LionCount is still in the plan, and that the answers equal the
# ones without the pushdown.
set -u

cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
PGDATA=${LION_PGDATA:-$ROOT/.local/data}
PG_CONFIG=${PG_CONFIG:-$ROOT/.local/pg/bin/pg_config}
case $PG_CONFIG in
	*/*) PG_CONFIG=$(cd "$(dirname "$PG_CONFIG")" && pwd)/$(basename "$PG_CONFIG") ;;
	*) PG_CONFIG=$(command -v "$PG_CONFIG") ;;
esac
BINDIR=$($PG_CONFIG --bindir)
LOG=$ROOT/.local/pg.log
MOD=$ROOT/test/modules/lion_hooktest
FAILED=""

# Only the state that was found is restored: see test/rmgr-check.sh.
if "$BINDIR/pg_ctl" -D "$PGDATA" status >/dev/null 2>&1; then
	WAS_RUNNING=1
else
	WAS_RUNNING=0
fi

restore()
{
	"$BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null 2>&1
	if [ "$WAS_RUNNING" = 1 ]; then
		echo "== restoring the cluster to generic mode"
		"$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" start >/dev/null 2>&1
	else
		echo "== the cluster was stopped when this began; leaving it stopped"
	fi
}
trap restore EXIT

# STOP and START, never restart: see test/rmgr-check.sh.
start_with()
{
	"$BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null 2>&1
	if [ -n "$1" ]; then
		"$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" -o "$1" start >/dev/null
	else
		"$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" start >/dev/null
	fi
}

# run <label> <tests...>: one pg_regress run of the module's tests, in a
# freshly created database
run()
{
	local label=$1
	shift
	echo "== $label: $*"
	if make -s -C "$MOD" PG_CONFIG="$PG_CONFIG" installcheck REGRESS="$*"; then
		:
	else
		echo "== FAILED: $label (see $MOD/regression.diffs)"
		cp "$MOD/regression.diffs" "$MOD/regression.diffs.$(echo "$label" | tr -c 'a-z0-9\n' _)" 2>/dev/null
		FAILED="$FAILED [$label]"
	fi
}

full()
{
	[ "${HOOKCHECK_FULL:-0}" != "0" ] || return 0
	echo "== $1: make installcheck"
	make -C "$ROOT" PG_CONFIG="$PG_CONFIG" installcheck || FAILED="$FAILED [$1: installcheck]"
}

# HOOKCHECK_SKIP_INSTALL=1 for an installation this user cannot write to
# (CI's packaged servers), where the module is installed beforehand with sudo.
if [ "${HOOKCHECK_SKIP_INSTALL:-0}" = "0" ]; then
	echo "== building and installing test/modules/lion_hooktest"
	make -s -C "$MOD" PG_CONFIG="$PG_CONFIG" WERROR="${WERROR:-0}" install >/dev/null || exit 1
fi
rm -f "$MOD"/regression.diffs.*

eval "$("$ROOT/dev.sh" env)"

# ---- 1. nothing preloaded -------------------------------------------------
start_with "" || exit 1
run "no preload, table AMs" tableam
run "no preload, lion_hooktest LOADed first" hooks_setup hooks_lion_outer
run "no preload, pg_lion loaded first" hooks_setup hooks_hooktest_outer

# ---- 2. both preloaded, both orders ----------------------------------------
if start_with "-c shared_preload_libraries=pg_lion,lion_hooktest -c pg_lion.rmgr_id=129"; then
	run "preload pg_lion,lion_hooktest" hooks_setup hooks_hooktest_outer hooks_rmgr
	full "preload pg_lion,lion_hooktest"
else
	FAILED="$FAILED [preload pg_lion,lion_hooktest: server did not start]"
fi
if start_with "-c shared_preload_libraries=lion_hooktest,pg_lion -c pg_lion.rmgr_id=129"; then
	run "preload lion_hooktest,pg_lion" hooks_setup hooks_lion_outer hooks_rmgr
	full "preload lion_hooktest,pg_lion"
else
	FAILED="$FAILED [preload lion_hooktest,pg_lion: server did not start]"
fi

# ---- 3. both resource managers on the same id ------------------------------
echo "== preload pg_lion,lion_hooktest, both on resource manager id 128"
before=$(wc -c < "$LOG")
if start_with "-c shared_preload_libraries=pg_lion,lion_hooktest"; then
	FAILED="$FAILED [rmgr id collision: the server started]"
elif tail -c +"$((before + 1))" "$LOG" |
		grep -q 'failed to register custom resource manager "lion_hooktest" with ID 128'; then
	echo "   refused, as it must be: $(tail -c +"$((before + 1))" "$LOG" | grep -m1 -A1 'failed to register' | tr '\n' ' ')"
else
	FAILED="$FAILED [rmgr id collision: no registration error in $LOG]"
fi

if [ -z "$FAILED" ]; then
	echo "== hookcheck passed"
	exit 0
fi
echo "== hookcheck FAILED:$FAILED"
exit 1
