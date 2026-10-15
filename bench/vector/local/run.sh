#!/bin/bash
# The DBpedia benchmark (knn_dbpedia.py) end to end on this machine: fetch the
# shards, prepare the queries and the truth, load, build the four indexes, run
# each warm, run them cold, and write the tables.  Each step leaves a marker
# in $BENCH_DIR/state, so a run that stops resumes where it stopped; delete a
# marker (or the state directory) to redo a step.
#   run.sh                 every step
#   STEPS="warm-lionvec cold table" run.sh    just those
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
LISTS=${LISTS:-$("$PY" -c "print(round($ROWS ** 0.5))")}   # VectorChord's lists: sqrt(n), as 339 at 115k

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
export KNN_EF=${KNN_EF:-20,40,70,100,200,400,800}
export KNN_PROBES=${KNN_PROBES:-10,20,40,80,160,320,640}

STEPS=${STEPS:-"fetch prepare load lionvec hnsw vchord diskann warm-lionvec warm-hnsw warm-vchord warm-diskann cold table"}

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
		echo "settings:"; grep -A40 '^# lionbench' "$PGDATA_DIR/postgresql.conf" | sed 's/^/  /'
	} > "$OUT/machine.txt"
}

"$LOCAL/pg.sh" status >/dev/null 2>&1 || "$LOCAL/pg.sh" start
machine
KINDS=(lionvec hnsw)
has_ext vchord && KINDS+=(vchord) || echo "VectorChord is not installed: its steps are skipped"
has_ext vectorscale && KINDS+=(diskann) || echo "pgvectorscale is not installed: diskann's steps are skipped"
want() { [[ " $STEPS " == *" $1 "* ]]; }
kind_ok() { [[ " ${KINDS[*]} " == *" $1 "* ]]; }

want fetch   && step fetch "$PY" "$LOCAL/fetch.py" "$BENCH_DIR/shards" "$NSHARDS"
want prepare && step prepare $KNN prepare
want load    && step load $KNN load
want lionvec && step lionvec $KNN lionvec 0
want hnsw    && step hnsw $KNN hnsw
want vchord  && kind_ok vchord && step vchord $KNN vchord "$LISTS"
want diskann && kind_ok diskann && step diskann $KNN diskann
for k in "${KINDS[@]}"; do
	want "warm-$k" && step "warm-$k" $KNN warm "$k" "$k"
done
if want cold; then
	sh -c "$KNN_DROP" || { echo "cannot drop the page cache ($KNN_DROP): run setup.sh"; exit 1; }
	export KNN_COLD_KINDS=$(IFS=,; echo "${KINDS[*]}")
	step cold $KNN cold cold
fi
if want table; then
	files=(); for k in "${KINDS[@]}"; do [ -e "$OUT/$k.json" ] && files+=("$OUT/$k.json"); done
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
