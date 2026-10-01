#!/bin/bash
# Run one or more of the finding reproducers in this directory against the
# running cluster PGHOST/PGPORT name (eval "$(./dev.sh env)" first):
#
#   test/soak/findings/run-spec.sh wagg_aborted_insert
#
# A .spec runs under pg_isolation_regress against expected/<name>.out, which
# holds the CORRECT answers: the run fails (and prints the diff) while the
# finding is there.  A .sql runs under psql and prints its own verdict.
# The specs need the injection_points module (a server configured with
# --enable-injection-points).
set -u
D=$(cd "$(dirname "$0")" && pwd)
PG_CONFIG=${PG_CONFIG:-$D/../../../.local/pg/bin/pg_config}
PGXS_DIR=$(dirname "$("$PG_CONFIG" --pgxs)")
OUT=${FINDINGS_OUT:-$(mktemp -d)}
rc=0
[ $# -gt 0 ] || set -- $(cd "$D" && ls *.spec *.sql 2>/dev/null | sed 's/\.\(spec\|sql\)$//' | sort -u)
for t in "$@"; do
	if [ -f "$D/$t.spec" ]; then
		"$PGXS_DIR/../../src/test/isolation/pg_isolation_regress" \
			--inputdir="$D" --outputdir="$OUT" --use-existing \
			--dbname=lion_findings "$t" || { rc=1; cat "$OUT/regression.diffs" 2>/dev/null; }
	elif [ -f "$D/$t.sql" ]; then
		psql -X -v ON_ERROR_STOP=1 -f "$D/$t.sql" || rc=1
	else
		echo "no $t.spec or $t.sql in $D" >&2; rc=2
	fi
done
echo "outputs in $OUT"
exit $rc
