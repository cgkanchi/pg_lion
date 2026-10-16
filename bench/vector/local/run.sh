#!/bin/bash
# The DBpedia benchmark (knn_dbpedia.py) end to end on this machine: fetch the
# shards, prepare the queries and the truth, load, build the indexes, run each
# warm, run them cold, and write the tables.  lionvec against VectorChord by
# default; KINDS="lionvec vchord hnsw diskann" adds the other two.  Each step
# leaves a marker in $BENCH_DIR/state, so a run that stops resumes where it
# stopped; delete a marker (or the state directory) to redo a step.
#   run.sh                              every step
#   STEPS="warm cold table" run.sh      just those (of fetch prepare load
#                                       build warm cold table)
# Results: bench/results/<date>-dbpedia-local-<rows>/ in this checkout.
set -euo pipefail
. "$(dirname "$0")/env.sh"
PY=$VENV/bin/python
KNN="$PY $REPO/bench/vector/knn_dbpedia.py"
STATE=$BENCH_DIR/state
mkdir -p "$STATE" "$BENCH_DIR/logs"

ROWS=$(( NSHARDS * 38462 ))      # every shard but the last holds 38,462 rows
[ "$NSHARDS" -eq 26 ] && ROWS=1000000
if [ -s "$STATE/out" ]; then OUT=$(cat "$STATE/out"); else
	OUT=$REPO/bench/results/$(date -u +%F)-dbpedia-local-$(( ROWS / 1000 ))k
	echo "$OUT" > "$STATE/out"
fi
# VectorChord's lists: sqrt(n), 1,000 at 1M (339 at 115k, the better of the
# two counts tried there); LISTS=4000 run.sh STEPS="build warm cold table"
# afterwards adds another count (the build replaces the index)
LISTS=${LISTS:-$("$PY" -c "print(round($ROWS ** 0.5))")}

export KNN_DSN="dbname=dbpedia" KNN_ADMIN_DSN="dbname=postgres"
export KNN_CACHE=$BENCH_DIR/cache KNN_OUT=$OUT KNN_SRC=$REPO
export KNN_SHARDS="$BENCH_DIR/shards/train-*-of-*.parquet" KNN_NSHARDS=$NSHARDS
# past ~200k rows the table's TOAST no longer fits shared_buffers: the index
# goes there, the table and TOAST into the kernel's cache
[ "$ROWS" -gt 200000 ] && export KNN_PREWARM=${KNN_PREWARM:-read}
export KNN_RESTART="$LOCAL/pg.sh restart"
export KNN_DROP=${KNN_DROP:-"sudo -n /usr/local/sbin/lionbench-drop-caches"}
export KNN_COLD_FROM=$OUT
# wider sweeps than at 115k, since a million rows need more for a recall
export KNN_MARGINS=${KNN_MARGINS:-2,3,4,6,8,12,16,24,32}
export KNN_PROBES=${KNN_PROBES:-10,20,40,80,160,320,640}
# parallel builds, the same for every kind (knn_dbpedia.py): half the CPU
# threads, at most 8; KNN_BUILD_WORKERS=0 builds serially, as at 115k
export KNN_BUILD_WORKERS=${KNN_BUILD_WORKERS:-$(( $(nproc) / 2 > 8 ? 8 : $(nproc) / 2 ))}
export KNN_EF=${KNN_EF:-20,40,70,100,200,400,800}

STEPS=${STEPS:-"fetch prepare load build warm cold table"}
read -r -a KINDS <<< "${KINDS:-lionvec vchord}"

has_ext() {   # is the extension installed?
	[ -n "$(psql -XAtq -d postgres -c "SELECT 1 FROM pg_available_extensions WHERE name = '$1'")" ]
}

step() {      # step NAME COMMAND...: run it once, logged
	local name=$1; shift
	if [ -e "$STATE/$name.done" ]; then echo "== $name: done before (rm $STATE/$name.done to redo)"; return; fi
	echo "== $name: $* ($(date -u +%H:%M:%S) UTC)"
	local t=$SECONDS
	# (set -e does not reach into a function called from an && list, so the
	# failure is checked here)
	if ! ( cd "$REPO" && "$@" ) 2>&1 | tee "$BENCH_DIR/logs/$name.log"; then
		echo "== $name failed: $BENCH_DIR/logs/$name.log"; exit 1
	fi
	echo "== $name: $(( (SECONDS - t) / 60 )) min"
	touch "$STATE/$name.done"
}

machine() {
	mkdir -p "$OUT"
	{
		echo "date: $(date -u +%FT%TZ)"
		echo "rows: $ROWS ($NSHARDS shards)"
		echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-) ($(nproc) threads)"
		echo "memory: $(awk '/MemTotal/ {printf "%.1f GB", $2 / 1048576}' /proc/meminfo) visible to Linux"
		echo "kernel: $(uname -r)"
		grep -qi microsoft /proc/version && echo "wsl: yes"
		echo "disk: $(df -h --output=source,fstype,size "$BENCH_DIR" | tail -1)"
		echo "postgres: $(postgres --version)"
		echo "pg_lion: $(git -C "$REPO" rev-parse --short HEAD)"
		echo "build workers: $KNN_BUILD_WORKERS"
		echo "settings:"; grep -A40 '^# lionbench' "$PGDATA_DIR/postgresql.conf" | sed 's/^/  /'
	} > "$OUT/machine.txt"
}

"$LOCAL/pg.sh" status >/dev/null 2>&1 || "$LOCAL/pg.sh" start
machine
for k in "${KINDS[@]}"; do
	case $k in
	  lionvec | hnsw) ;;
	  vchord)  has_ext vchord || { echo "VectorChord is not installed: run setup.sh"; exit 1; } ;;
	  diskann) has_ext vectorscale || { echo "pgvectorscale is not installed: WITH_DISKANN=1 setup.sh"; exit 1; } ;;
	  *) echo "unknown kind $k (lionvec, vchord, hnsw, diskann)"; exit 1 ;;
	esac
done
want() { [[ " $STEPS " == *" $1 "* ]]; }
# a warm run's result file: VectorChord's carries its list count, so that a
# second run with another LISTS lands beside the first
name() { [ "$1" = vchord ] && echo "vchord-l$LISTS" || echo "$1"; }

want fetch   && step fetch "$PY" "$LOCAL/fetch.py" "$BENCH_DIR/shards" "$NSHARDS"
want prepare && step prepare $KNN prepare
want load    && step load $KNN load
if want build; then
	for k in "${KINDS[@]}"; do
		case $k in
		  lionvec) step build-lionvec $KNN lionvec 0 ;;
		  vchord)  step "build-vchord-l$LISTS" $KNN vchord "$LISTS" ;;
		  *)       step "build-$k" $KNN "$k" ;;
		esac
	done
fi
if want warm; then
	for k in "${KINDS[@]}"; do step "warm-$(name "$k")" $KNN warm "$k" "$(name "$k")"; done
fi
if want cold; then
	sh -c "$KNN_DROP" || { echo "cannot drop the page cache ($KNN_DROP): run setup.sh"; exit 1; }
	export KNN_COLD_KINDS=$(IFS=,; echo "${KINDS[*]}")
	step cold $KNN cold cold
fi
if want table; then
	files=(); for f in "$OUT"/*.json; do [ "$(basename "$f")" != cold.json ] && files+=("$f"); done
	( cd "$REPO" && $KNN table "${files[@]}" ) > "$OUT/tables.md"
	[ -e "$OUT/cold.json" ] && "$PY" - "$OUT/cold.json" >> "$OUT/tables.md" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print('\n| filter | method | median ms | pages read | cache dropped |\n| --- | --- | --- | --- | --- |')
for r in d['results']:
    print(f"| {r['sel']:g} | {r['name']} | {r['median_ms']:.0f} | {r['pages_read_median']:.0f} | {r['cache_dropped']} |")
PY
	( cd "$REPO" && $KNN sizes ) > "$OUT/sizes.txt" 2>&1 || true
	echo "== tables: $OUT/tables.md"
fi
echo "== results in $OUT"
