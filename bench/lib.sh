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
BENCH_STARTED=0          # we started the postmaster and must stop it
BENCH_VERIFIED=0         # SHOW data_directory matched the requested directory
BENCH_PREFIX=""
BENCH_DATA=""
BENCH_ORIG_INVALID=""    # fact_% indexes that were ALREADY invalid before this run

bench_psql() { "$BENCH_PREFIX/bin/psql" -X -q -v ON_ERROR_STOP=1 "$@"; }

# Put back exactly the indexes this run flipped: every fact_% index that is
# invalid now and was not invalid when the run began.  Never runs against an
# endpoint whose identity has not been verified.
bench_restore_indexes() {
	[ "$BENCH_VERIFIED" = 1 ] || return 0
	local excl=""
	[ -n "$BENCH_ORIG_INVALID" ] && excl="and indexrelid::regclass::text not in ($BENCH_ORIG_INVALID)"
	bench_psql -c "update pg_index set indisvalid = true where indexrelid::regclass::text like 'fact\\_%' and not indisvalid $excl" >/dev/null 2>&1 || true
}

bench_cleanup() {
	local rc=$?
	trap - EXIT
	if [ "$BENCH_STARTED" = 1 ]; then
		bench_restore_indexes
		"$BENCH_PREFIX/bin/pg_ctl" -D "$BENCH_DATA" stop -m fast >/dev/null 2>&1 || true
	fi
	exit $rc
}

# bench_start_cluster <prefix> <datadir>
#
# Order matters: nothing that can reach a server is armed until we know which
# server it is.  The trap is installed only once our own postmaster is up, and
# catalog writes are gated on the identity check below.
bench_start_cluster() {
	BENCH_PREFIX=$1; BENCH_DATA=$(cd "$2" && pwd)
	mkdir -p "$BENCH_SOCKDIR"
	export PGHOST=$BENCH_SOCKDIR PGPORT=$BENCH_PORT PGUSER=postgres PGDATABASE=postgres
	if "$BENCH_PREFIX/bin/pg_ctl" -D "$BENCH_DATA" status >/dev/null 2>&1; then
		echo "cluster at $BENCH_DATA is already running; refusing to guess its endpoint" >&2
		exit 1
	fi
	if [ -S "$BENCH_SOCKDIR/.s.PGSQL.$BENCH_PORT" ]; then
		echo "something already listens on $BENCH_SOCKDIR:$BENCH_PORT; refusing to share the endpoint" >&2
		exit 1
	fi
	"$BENCH_PREFIX/bin/pg_ctl" -D "$BENCH_DATA" -l "$BENCH_DATA/../bench_pg.log" \
		-o "-p $BENCH_PORT -k $BENCH_SOCKDIR -c listen_addresses=''" -w start >/dev/null 2>&1 \
		|| { echo "could not start the benchmark cluster at $BENCH_DATA (see $BENCH_DATA/../bench_pg.log)" >&2; exit 1; }
	BENCH_STARTED=1
	trap bench_cleanup EXIT
	local actual
	actual=$(bench_psql -tA -c "show data_directory" 2>/dev/null || true)
	if [ -z "$actual" ] || [ "$(cd "$actual" 2>/dev/null && pwd)" != "$BENCH_DATA" ]; then
		echo "connected to a cluster with data_directory '$actual', expected '$BENCH_DATA'; aborting" >&2
		exit 1
	fi
	BENCH_VERIFIED=1
	BENCH_ORIG_INVALID=$(bench_psql -tA -c "select coalesce(string_agg(quote_literal(indexrelid::regclass::text), ','), '') from pg_index where indexrelid::regclass::text like 'fact\\_%' and not indisvalid" 2>/dev/null || true)
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
