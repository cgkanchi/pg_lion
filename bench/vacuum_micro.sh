#!/bin/bash
#
# bench/vacuum_micro.sh -- what a VACUUM of a lion index costs (DESIGN.md §18).
#
# Two workloads, both against btree as the control:
#
#   portfolio  1M rows, eight single-column indexes with the column mix of
#              bench/comprehensive/workloads.py, `DELETE ... WHERE id % 100 = 1`
#              and one VACUUM.  Reports VACUUM wall time, the WAL the VACUUM
#              wrote (total / full-page images / by resource manager, from
#              pg_walinspect when it is installed), and the DEBUG1 breakdown
#              lion_vacuum.c emits (cleanup-lock waits, container filtering,
#              GenericXLog apply, INLINE entry rewrites).
#
#   churn      100k rows, a unique bigint key and a two-valued key, five cycles
#              of `UPDATE fact SET k = k + n` followed by VACUUM.  Reports per
#              cycle: VACUUM time and WAL, index size, and the entry count and
#              deleted-page count of lion_index_stats().
#
# Usage: bench/vacuum_micro.sh [--db NAME] [--label TAG] [--rows N] [--cycles N]
#                              [--only portfolio|churn] [--am lion|btree]
#
# Results land in bench/results/<label>/ as TSV plus the raw psql log.  The
# numbers are only ever comparable WITHIN one run of this script: the dev
# cluster is an assert-enabled build with fsync off.
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DB=lion_vacbench
LABEL=$(date +%Y-%m-%d-%H%M%S)
ROWS=1000000
CHURN_ROWS=100000
CYCLES=5
ONLY=all
AMS="lion btree"

while [ $# -gt 0 ]; do
	case "$1" in
		--db) DB=$2; shift 2 ;;
		--label) LABEL=$2; shift 2 ;;
		--rows) ROWS=$2; shift 2 ;;
		--churn-rows) CHURN_ROWS=$2; shift 2 ;;
		--cycles) CYCLES=$2; shift 2 ;;
		--only) ONLY=$2; shift 2 ;;
		--am) AMS=$2; shift 2 ;;
		-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
		*) echo "unknown option $1" >&2; exit 1 ;;
	esac
done

eval "$("$ROOT/dev.sh" env)"

OUT=$ROOT/bench/results/$LABEL
mkdir -p "$OUT"
LOG=$OUT/psql.log

psql -X -q -d postgres -c "SELECT 1" >/dev/null
psql -X -q -d postgres -tAc \
	"SELECT 1 FROM pg_database WHERE datname = '$DB'" | grep -q 1 ||
	createdb "$DB"

# psql wrapper: everything this script runs goes into the log as well.
run() { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" "$@" 2>&1 | tee -a "$LOG"; }
val() { psql -X -q -v ON_ERROR_STOP=1 -tA -d "$DB" -c "$1"; }

run -c "CREATE EXTENSION IF NOT EXISTS pg_lion" >/dev/null
HAVE_WALINSPECT=no
if run -c "CREATE EXTENSION IF NOT EXISTS pg_walinspect" >/dev/null 2>&1; then
	HAVE_WALINSPECT=yes
fi

echo "# label=$LABEL db=$DB rows=$ROWS churn_rows=$CHURN_ROWS cycles=$CYCLES walinspect=$HAVE_WALINSPECT"
echo "# commit=$(cd "$ROOT" && git rev-parse --short HEAD)$(cd "$ROOT" && git diff --quiet || echo '+dirty')"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# WAL written between two LSNs, split into total / FPI bytes / top resource
# managers.  Without pg_walinspect only the total (from pg_wal_lsn_diff) is
# available, which is what the caller has already measured.
wal_breakdown() {
	local lo=$1 hi=$2 per=${3:-false}
	[ "$HAVE_WALINSPECT" = yes ] || { echo ""; return; }
	val "SELECT coalesce(string_agg(format('%s %s/%sfpi/%s', kind,
	            pg_size_pretty(combined_size), pg_size_pretty(fpi_size), count), '; '
	            ORDER BY combined_size DESC), '')
	     FROM (SELECT \"resource_manager/record_type\" AS kind, count,
	                  fpi_size, combined_size
	           FROM pg_get_wal_stats('$lo', least('$hi', pg_current_wal_flush_lsn()), $per)
	           WHERE count > 0 ORDER BY combined_size DESC LIMIT 6) s" 2>/dev/null || echo ""
}

# ---------------------------------------------------------------------------
# Workload 1: the eight-index portfolio
# ---------------------------------------------------------------------------

portfolio_one() {
	local am=$1
	local tsv=$OUT/portfolio.tsv

	run -c "DROP TABLE IF EXISTS fact" >/dev/null
	run <<-SQL >/dev/null
		SET synchronous_commit = on;
		CREATE TABLE fact AS
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
		FROM generate_series(1::bigint,$ROWS::bigint) i;
	SQL

	local col
	for col in c2 c20 c200 c20k c1m clustered skew nullable; do
		run -c "CREATE INDEX ix_$col ON fact USING $am ($col)" >/dev/null
	done

	run <<-SQL >/dev/null
		SET synchronous_commit = on;
		VACUUM (FREEZE, ANALYZE) fact;
		CHECKPOINT;
	SQL

	local idxsize_before
	idxsize_before=$(val "SELECT pg_indexes_size('fact')")

	run -c "SET synchronous_commit=on; DELETE FROM fact WHERE id % 100 = 1" >/dev/null
	run -c "CHECKPOINT" >/dev/null

	local lsn0 lsn1 t0 t1 ms wal
	lsn0=$(val "SELECT pg_current_wal_insert_lsn()")
	t0=$(date +%s%N)
	# DEBUG1 is where lion_vacuum.c's cost breakdown comes out.
	psql -X -q -v ON_ERROR_STOP=1 -d "$DB" \
		-c "SET client_min_messages=debug1" -c "SET synchronous_commit=on" -c "VACUUM (ANALYZE) fact" \
		>"$OUT/portfolio-$am.debug" 2>&1 || { cat "$OUT/portfolio-$am.debug"; exit 1; }
	t1=$(date +%s%N)
	lsn1=$(val "SELECT pg_current_wal_insert_lsn()")
	ms=$(( (t1 - t0) / 1000000 ))
	wal=$(val "SELECT pg_wal_lsn_diff('$lsn1','$lsn0')::bigint")

	local idxsize_after
	idxsize_after=$(val "SELECT pg_indexes_size('fact')")

	[ -s "$tsv" ] || printf 'am\tvacuum_ms\twal_bytes\tidx_bytes_before\tidx_bytes_after\twal_by_rmgr\n' >"$tsv"
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$am" "$ms" "$wal" "$idxsize_before" "$idxsize_after" \
		"$(wal_breakdown "$lsn0" "$lsn1")" >>"$tsv"

	echo "portfolio/$am: VACUUM ${ms} ms, WAL $(val "SELECT pg_size_pretty($wal::bigint)"), indexes $(val "SELECT pg_size_pretty($idxsize_after::bigint)")"
	echo "    wal by rmgr:   $(wal_breakdown "$lsn0" "$lsn1")"
	echo "    wal by record: $(wal_breakdown "$lsn0" "$lsn1" true)"
	if [ "$am" = lion ]; then
		grep -h 'lion vacuum' "$OUT/portfolio-$am.debug" | sed 's/^/    /' || true
		val "SELECT format('    stats %s: entries=%s container_pages=%s containers=%s ntids=%s bytes=%s slack=%s free=%s',
		         c.relname, s.entries, s.container_pages, s.containers, s.ntids,
		         s.container_bytes, s.slack_bytes, s.free_bytes)
		     FROM pg_class c, lion_index_stats(c.oid) s
		     WHERE c.relname LIKE 'ix_%' ORDER BY c.relname"
	fi
	run -c "DROP TABLE fact" >/dev/null
}

# ---------------------------------------------------------------------------
# Workload 2: changing-key churn (bench/comprehensive/stress.py's churn loop)
# ---------------------------------------------------------------------------

churn_one() {
	local am=$1
	local tsv=$OUT/churn.tsv
	local n=$CHURN_ROWS

	run -c "DROP TABLE IF EXISTS fact" >/dev/null
	run <<-SQL >/dev/null
		SET synchronous_commit = on;
		CREATE TABLE fact AS
		SELECT i::bigint AS k, (i%2)::int AS h FROM generate_series(1,$n) i;
		CREATE INDEX ON fact USING $am (k);
		CREATE INDEX ON fact USING $am (h);
		VACUUM (FREEZE, ANALYZE) fact;
	SQL

	[ -s "$tsv" ] || printf 'am\tcycle\tvacuum_ms\twal_bytes\tidx_bytes\tentries_k\tentries_h\tdeleted_pages\n' >"$tsv"

	local cycle
	for cycle in $(seq 1 "$CYCLES"); do
		run -c "CHECKPOINT" >/dev/null
		run -c "SET synchronous_commit=on; UPDATE fact SET k = k + $n" >/dev/null

		local lsn0 lsn1 t0 t1 ms wal
		lsn0=$(val "SELECT pg_current_wal_insert_lsn()")
		t0=$(date +%s%N)
		psql -X -q -v ON_ERROR_STOP=1 -d "$DB" \
			-c "SET client_min_messages=debug1" -c "SET synchronous_commit=on" -c "VACUUM (ANALYZE) fact" \
			>>"$OUT/churn-$am.debug" 2>&1 || { cat "$OUT/churn-$am.debug"; exit 1; }
		t1=$(date +%s%N)
		lsn1=$(val "SELECT pg_current_wal_insert_lsn()")
		ms=$(( (t1 - t0) / 1000000 ))
		wal=$(val "SELECT pg_wal_lsn_diff('$lsn1','$lsn0')::bigint")

		local size ek eh dp
		size=$(val "SELECT pg_indexes_size('fact')")
		if [ "$am" = lion ]; then
			ek=$(val "SELECT entries FROM lion_index_stats('fact_k_idx')")
			eh=$(val "SELECT entries FROM lion_index_stats('fact_h_idx')")
			dp=$(val "SELECT coalesce(sum(deleted_pages),0) FROM pg_class c, lion_index_stats(c.oid) s WHERE c.relname IN ('fact_k_idx','fact_h_idx')" 2>/dev/null || echo 0)
		else
			ek=-1; eh=-1; dp=-1
		fi

		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
			"$am" "$cycle" "$ms" "$wal" "$size" "$ek" "$eh" "$dp" >>"$tsv"
		echo "churn/$am cycle $cycle: VACUUM ${ms} ms, WAL $(val "SELECT pg_size_pretty($wal::bigint)"), indexes $(val "SELECT pg_size_pretty($size::bigint)"), entries k=$ek h=$eh, deleted pages=$dp"
	done

	run -c "DROP TABLE fact" >/dev/null
}

# ---------------------------------------------------------------------------

for am in $AMS; do
	case "$ONLY" in
		all|portfolio) portfolio_one "$am" ;;
	esac
done
for am in $AMS; do
	case "$ONLY" in
		all|churn) churn_one "$am" ;;
	esac
done

echo
echo "results in $OUT"
for f in "$OUT"/*.tsv; do
	[ -e "$f" ] || continue
	echo "--- $(basename "$f")"
	column -t -s "$(printf '\t')" "$f"
done
