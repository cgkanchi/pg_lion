#!/bin/bash
# ALTER EXTENSION ... UPDATE from every earlier version of each extension's
# install script, against a fresh install of the current one.
#
# Usage: test/upgrade/run.sh [extension ...]	(default: every extension the
# Makefile builds)
#
# For each version of <extension>--<version>.sql in the git history (so the
# checkout needs its full history: fetch-depth 0 in CI), it installs that
# script under its version in a new database, creates a lion (or lionvec)
# index on every operator class the extension then had, runs ALTER EXTENSION
# UPDATE, and requires the extension's catalog entries (test/upgrade/
# catalog.sql) to equal those of a fresh CREATE EXTENSION, and every index to
# REINDEX.  A version of the script that this server cannot install (one for
# another PostgreSQL major, or naming a C function the library no longer has)
# is skipped and reported; any other install failure fails, and the newest
# earlier version must install.
#
# It needs the current build installed (make install), a server to talk to
# (./dev.sh env sets PGHOST and friends), and write access to the server's
# extension directory, where it puts each old script for the length of its
# test.
set -u

cd "$(dirname "$0")/../.."
PG_CONFIG=${PG_CONFIG:-$(if [ -x .local/pg/bin/pg_config ]; then echo .local/pg/bin/pg_config; else echo pg_config; fi)}
EXTDIR=$("$PG_CONFIG" --sharedir)/extension
PSQL="psql -X -q -v ON_ERROR_STOP=1"
OUT=test/upgrade/out
rm -rf "$OUT"
mkdir -p "$OUT"

if [ $# -gt 0 ]; then
	exts="$*"
else
	exts=$(sed -n 's/^EXTENSION = //p' Makefile)
fi

if [ "$(git rev-parse --is-shallow-repository)" = true ]; then
	echo "test/upgrade/run.sh needs the full git history (git fetch --unshallow)" >&2
	exit 1
fi

failed=0
db=lion_upgrade_test

# the old install script put in the extension directory for the test under
# way, removed however the script ends
planted=
unplant() {
	[ -z "$planted" ] || rm -f "$planted"
	planted=
}
trap unplant EXIT

# Whether a CREATE EXTENSION error is one of the two this server is expected
# to give an old script: a C function the library no longer has, or one of the
# core functions below that some majors lack.  Anything else is a failure.
expected_install_error() {
	local err
	err=$(grep -m1 ERROR "$1")
	case "$err" in
	*'could not find function "'*'" in file'*) return 0 ;;
	*'ERROR:  function hashbool(boolean) does not exist') return 0 ;;
	esac
	return 1
}

# The extension's indexable operator classes, as "<column type>|<am>|<class>"
# lines, separated by |: polymorphic input types get a concrete type of their kind.
opclasses() {
	$PSQL -d "$1" -At -F'|' -c "
		SELECT CASE c.opcintype
				 WHEN 'anyarray'::regtype THEN 'int4[]'
				 WHEN 'anyenum'::regtype THEN 'lion_upgrade_enum'
				 ELSE CASE WHEN t.typname IN ('vector', 'halfvec')
						   THEN format_type(c.opcintype, NULL) || '(3)'
						   ELSE format_type(c.opcintype, NULL) END
			   END, quote_ident(a.amname), quote_ident(n.nspname) || '.' || quote_ident(c.opcname)
		FROM pg_depend d
		JOIN pg_extension x ON x.oid = d.refobjid AND x.extname = '$2'
		JOIN pg_opclass c ON d.classid = 'pg_opclass'::regclass AND c.oid = d.objid
		JOIN pg_am a ON a.oid = c.opcmethod
		JOIN pg_namespace n ON n.oid = c.opcnamespace
		JOIN pg_type t ON t.oid = c.opcintype
		WHERE d.deptype = 'e'
		ORDER BY c.opcname"
}

for ext in $exts; do
	cur=$(sed -n "s/^default_version = '\(.*\)'$/\1/p" "$ext.control")
	if [ -z "$cur" ]; then
		echo "$ext: no default_version in $ext.control" >&2
		failed=1
		continue
	fi

	dropdb --if-exists "$db" 2>/dev/null && createdb "$db" || exit 1
	if ! $PSQL -d "$db" -c "CREATE EXTENSION $ext CASCADE" 2> "$OUT/$ext-fresh.err"; then
		if grep -q 'is not available' "$OUT/$ext-fresh.err"; then
			echo "$ext: skipped, an extension it requires is not installed: $(head -1 "$OUT/$ext-fresh.err")"
			continue
		fi
		echo "$ext: CREATE EXTENSION failed:" >&2
		cat "$OUT/$ext-fresh.err" >&2
		failed=1
		continue
	fi
	$PSQL -d "$db" -v ext="$ext" -f test/upgrade/catalog.sql > "$OUT/$ext-fresh.cat" || exit 1

	newest=1
	declare -A done_blobs=()
	for commit in $(git log --format=%H -- "$ext.control" "$ext--*.sql"); do
		ver=$(git show "$commit:$ext.control" 2>/dev/null | sed -n "s/^default_version = '\(.*\)'$/\1/p")
		[ -n "$ver" ] || continue
		blob=$(git rev-parse -q --verify "$commit:$ext--$ver.sql") || continue
		[ -z "${done_blobs[$ver:$blob]:-}" ] || continue
		done_blobs[$ver:$blob]=1
		# the current version's own script is the fresh install; an earlier
		# commit's script under the current version name is what
		# test/upgrade/lint.py refuses on a pull request
		[ "$ver" != "$cur" ] || continue

		tag="$ext-$ver-${commit:0:8}"
		if [ -e "$EXTDIR/$ext--$ver.sql" ]; then
			echo "$tag: $EXTDIR/$ext--$ver.sql already exists; is an old build installed?" >&2
			failed=1
			continue
		fi
		planted="$EXTDIR/$ext--$ver.sql"
		git cat-file blob "$blob" > "$planted"

		dropdb --if-exists "$db" 2>/dev/null && createdb "$db" || exit 1
		if ! $PSQL -d "$db" -c "CREATE EXTENSION $ext VERSION '$ver' CASCADE" 2> "$OUT/$tag.err"; then
			unplant
			if [ $newest = 1 ] || ! expected_install_error "$OUT/$tag.err"; then
				echo "$tag: does not install:" >&2
				cat "$OUT/$tag.err" >&2
				failed=1
			else
				echo "$tag: skipped, does not install on this server: $(grep -m1 ERROR "$OUT/$tag.err")"
			fi
			newest=0
			continue
		fi
		newest=0

		# an index on every operator class it has, so that the update meets
		# families in use
		$PSQL -d "$db" -c "CREATE TYPE lion_upgrade_enum AS ENUM ('a', 'b')" || exit 1
		i=0
		indexes=0
		while IFS='|' read -r type am class; do
			i=$((i + 1))
			if $PSQL -d "$db" -c "CREATE TABLE t$i (c $type)" \
					-c "CREATE INDEX t${i}_idx ON t$i USING $am (c $class)" 2>> "$OUT/$tag.idx.err"; then
				indexes=$((indexes + 1))
			fi
		done < <(opclasses "$db" "$ext")
		if [ $indexes -ne $i ]; then
			echo "$tag: $((i - indexes)) of $i operator classes could not be indexed:" >&2
			cat "$OUT/$tag.idx.err" >&2
			unplant
			failed=1
			continue
		fi

		if ! $PSQL -d "$db" -c "ALTER EXTENSION $ext UPDATE" 2> "$OUT/$tag.update"; then
			echo "$tag: ALTER EXTENSION UPDATE failed:" >&2
			cat "$OUT/$tag.update" >&2
			unplant
			failed=1
			continue
		fi
		unplant

		$PSQL -d "$db" -v ext="$ext" -f test/upgrade/catalog.sql > "$OUT/$tag.cat" || exit 1
		if ! diff -u "$OUT/$ext-fresh.cat" "$OUT/$tag.cat" > "$OUT/$tag.diff"; then
			echo "$tag: the catalogs after the update differ from a fresh install of $cur:" >&2
			cat "$OUT/$tag.diff" >&2
			failed=1
			continue
		fi
		if ! $PSQL -d "$db" -c "REINDEX DATABASE $db" 2> "$OUT/$tag.reindex"; then
			echo "$tag: REINDEX after the update failed:" >&2
			cat "$OUT/$tag.reindex" >&2
			failed=1
			continue
		fi
		echo "$tag: ok ($i operator classes indexed$(grep -q WARNING "$OUT/$tag.update" && echo "; $(grep -c WARNING "$OUT/$tag.update") warnings, in $OUT/$tag.update"))"
	done
	unset done_blobs
done

dropdb --if-exists "$db" 2>/dev/null
exit $failed
