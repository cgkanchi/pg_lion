#!/bin/bash
#
# Run the regression and isolation suites against the dev cluster with the
# custom WAL resource manager registered (DESIGN.md §25), and put the cluster
# back the way it was afterwards.
#
#   make installcheck-rmgr                  the whole suite in rmgr mode
#   make installcheck-rmgr WAL_CONSISTENCY=1  ... with every page replayed and
#                                              compared, which is the proof
#                                              that redo reproduces them
#
# The resource manager can only be registered from shared_preload_libraries,
# and a preloaded library is loaded once at postmaster start - so this
# restarts the cluster rather than SETting anything.  The options are passed
# on pg_ctl's command line instead of being written into postgresql.conf, so
# an interrupted run leaves no trace: the next plain restart is back in
# generic mode.
#
# Nothing here touches the cluster's data directory beyond starting and
# stopping it, and it never runs initdb.
set -u

cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
PGDATA=${LION_PGDATA:-$ROOT/.local/data}
PG_CONFIG=${PG_CONFIG:-$ROOT/.local/pg/bin/pg_config}
BINDIR=$($PG_CONFIG --bindir)
LOG=$ROOT/.local/pg.log

OPTS="-c shared_preload_libraries=pg_lion"
MODE="rmgr"
if [ "${WAL_CONSISTENCY:-0}" != "0" ]; then
	OPTS="$OPTS -c wal_consistency_checking=pg_lion"
	MODE="rmgr + wal_consistency_checking"
fi

# STOP and START rather than restart, in both directions.  `pg_ctl restart`
# reuses the options in postmaster.opts, so a restart after an "-o" start
# keeps the preload - which would leave the dev cluster in rmgr mode and make
# the next plain `make installcheck` silently test the wrong thing.
restore()
{
	echo "== restoring the cluster to generic mode"
	"$BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null 2>&1
	"$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" start >/dev/null 2>&1
	"$BINDIR/psql" -X -q -tAc \
		"select 'resource managers registered: ' || count(*) from pg_get_wal_resource_managers() where rm_name = 'pg_lion'"
}
trap restore EXIT

echo "== restarting the dev cluster with: $OPTS"
"$BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null 2>&1
"$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" -o "$OPTS" start || exit 1

eval "$("$ROOT/dev.sh" env)"

"$BINDIR/psql" -X -q -c \
	"SELECT rm_id, rm_name FROM pg_get_wal_resource_managers() WHERE rm_name = 'pg_lion'" \
	|| exit 1

echo "== make installcheck ($MODE)"
make -C "$ROOT" PG_CONFIG="$PG_CONFIG" installcheck
rc=$?

if [ $rc -eq 0 ]; then
	echo "== installcheck passed in $MODE mode"
else
	echo "== installcheck FAILED in $MODE mode (see test/regression.diffs)"
fi
exit $rc
