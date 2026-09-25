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
# stopping it, and it never runs initdb.  It leaves the cluster as it found
# it: running (in generic mode, with no pg_ctl -o options, which is how
# dev.sh starts it) if it was running, stopped if it was stopped.
#
# Before the suite runs, it proves that the restart took: the server must list
# pg_lion among its WAL resource managers, and an index built there must
# report wal_mode 'rmgr'.  Without that check a restart that silently came up
# without the preload would still pass - walrecords has an expected file for
# each mode, and pg_regress accepts either - and "passed in rmgr mode" would
# be a statement about generic mode.
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

# Whether the cluster was running when this began: that, and only that, is
# the state it is put back in.  pg_ctl status exits 0 for a running server.
if "$BINDIR/pg_ctl" -D "$PGDATA" status >/dev/null 2>&1; then
	WAS_RUNNING=1
else
	WAS_RUNNING=0
fi

# STOP and START rather than restart, in both directions.  `pg_ctl restart`
# reuses the options in postmaster.opts, so a restart after an "-o" start
# keeps the preload - which would leave the dev cluster in rmgr mode and make
# the next plain `make installcheck` silently test the wrong thing.
restore()
{
	"$BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null 2>&1
	if [ "$WAS_RUNNING" = 1 ]; then
		echo "== restoring the cluster to generic mode"
		"$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" start >/dev/null 2>&1
		"$BINDIR/psql" -X -q -tAc \
			"select 'resource managers registered: ' || count(*) from pg_get_wal_resource_managers() where rm_name = 'pg_lion'"
	else
		echo "== the cluster was stopped when this began; leaving it stopped"
	fi
}
trap restore EXIT

echo "== restarting the dev cluster with: $OPTS"
"$BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null 2>&1
"$BINDIR/pg_ctl" -D "$PGDATA" -l "$LOG" -o "$OPTS" start || exit 1

eval "$("$ROOT/dev.sh" env)"

# The proof that this is rmgr mode, before anything is run in it.  The index
# is built in a transaction that is rolled back, so the database is left as
# it was, the extension included.
registered=$("$BINDIR/psql" -X -q -tAc \
	"SELECT count(*) FROM pg_get_wal_resource_managers() WHERE rm_name = 'pg_lion'") || exit 1
if [ "$registered" != 1 ]; then
	echo "== FAILED: the restarted server does not list pg_lion among its WAL resource managers" >&2
	exit 1
fi
walmode=$("$BINDIR/psql" -X -q -tA -v ON_ERROR_STOP=1 <<-'SQL'
	SET client_min_messages = warning;
	BEGIN;
	CREATE EXTENSION IF NOT EXISTS pg_lion;
	CREATE TABLE lion_rmgr_check (k int);
	CREATE INDEX lion_rmgr_check_k ON lion_rmgr_check USING lion (k);
	SELECT lion_index_wal_mode('lion_rmgr_check_k');
	ROLLBACK;
SQL
) || exit 1
if [ "$walmode" != "rmgr" ]; then
	echo "== FAILED: an index built on the restarted server is in '$walmode' mode, not rmgr" >&2
	exit 1
fi
echo "== pg_lion is a registered resource manager, and a new index is built in rmgr mode"

echo "== make installcheck ($MODE)"
make -C "$ROOT" PG_CONFIG="$PG_CONFIG" installcheck
rc=$?

if [ $rc -eq 0 ]; then
	echo "== installcheck passed in $MODE mode"
else
	echo "== installcheck FAILED in $MODE mode (see test/regression.diffs)"
fi
exit $rc
