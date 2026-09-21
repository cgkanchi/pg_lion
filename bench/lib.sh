#!/bin/bash
# Shared helpers for the benchmark launchers.  Source this file.
#
# The launchers run destructive SQL (DROP EXTENSION ... CASCADE, flipping
# pg_index.indisvalid), so they must never reach a cluster other than the one
# whose data directory was requested.  Rules: start the cluster on a private
# socket directory and port that nothing else uses, fail if it does not start,
# verify SHOW data_directory before the first statement, stop it on exit if we
# started it, and put every fact_% index back to indisvalid = true on exit.

BENCH_SOCKDIR=${BENCH_SOCKDIR:-/tmp/claude-1000/pgsk_bench}
BENCH_PORT=${BENCH_PORT:-54331}
BENCH_STARTED=0
BENCH_PREFIX=""
BENCH_DATA=""

bench_psql() { "$BENCH_PREFIX/bin/psql" -X -q -v ON_ERROR_STOP=1 "$@"; }

bench_restore_indexes() {
	# bench.sh flips indisvalid to isolate index types; put every fact_% index back.
	bench_psql -c "update pg_index set indisvalid = true where indexrelid::regclass::text like 'fact\\_%' and not indisvalid" >/dev/null 2>&1 || true
}

bench_cleanup() {
	local rc=$?
	if [ -n "$BENCH_PREFIX" ]; then
		bench_restore_indexes
		if [ "$BENCH_STARTED" = 1 ]; then
			"$BENCH_PREFIX/bin/pg_ctl" -D "$BENCH_DATA" stop -m fast >/dev/null 2>&1 || true
		fi
	fi
	exit $rc
}

# bench_start_cluster <prefix> <datadir>
bench_start_cluster() {
	BENCH_PREFIX=$1; BENCH_DATA=$(cd "$2" && pwd)
	mkdir -p "$BENCH_SOCKDIR"
	export PGHOST=$BENCH_SOCKDIR PGPORT=$BENCH_PORT PGUSER=postgres PGDATABASE=postgres
	trap bench_cleanup EXIT
	if "$BENCH_PREFIX/bin/pg_ctl" -D "$BENCH_DATA" status >/dev/null 2>&1; then
		echo "cluster at $BENCH_DATA is already running; refusing to guess its endpoint" >&2
		exit 1
	fi
	"$BENCH_PREFIX/bin/pg_ctl" -D "$BENCH_DATA" -l "$BENCH_DATA/../bench_pg.log" \
		-o "-p $BENCH_PORT -k $BENCH_SOCKDIR -c listen_addresses=''" -w start >/dev/null 2>&1 \
		|| { echo "could not start the benchmark cluster at $BENCH_DATA (see $BENCH_DATA/../bench_pg.log)" >&2; exit 1; }
	BENCH_STARTED=1
	local actual
	actual=$(bench_psql -tA -c "show data_directory" 2>/dev/null || true)
	if [ "$(cd "$actual" 2>/dev/null && pwd)" != "$BENCH_DATA" ]; then
		echo "connected to a cluster with data_directory '$actual', expected '$BENCH_DATA'; aborting" >&2
		exit 1
	fi
}

# bench_build_extension <prefix> <project dir>  -> builds a clean copy against <prefix>
bench_build_extension() {
	local prefix=$1 p=$2 b
	b=$(mktemp -d /tmp/claude-1000/rbi_bench_build.XXXX)
	cp -r "$p/src" "$p/Makefile" "$p"/roaring_index*.control "$p"/roaring_index*--*.sql "$b/"
	rm -f "$b"/src/*.o "$b"/src/*.bc "$b"/*.so   # never reuse objects built against another server
	mkdir -p "$b/test/sql" "$b/test/isolation"
	( cd "$b" && make -s PG_CONFIG="$prefix/bin/pg_config" && make -s PG_CONFIG="$prefix/bin/pg_config" install ) \
		|| { echo "extension build failed in $b" >&2; exit 1; }
	echo "extension installed into $prefix (build dir $b)"
}
