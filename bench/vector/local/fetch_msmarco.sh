#!/bin/bash
# fetch_msmarco.sh DIR ROWS: CohereLabs/msmarco-v2.1-embed-english-v3's
# queries and as many of its passages_npy files, in order, as hold ROWS
# passages (26 files, 104 GB, for 50M), into DIR/queries and DIR/npy.
# Resumable: a finished file is kept, a partial one continued.
set -euo pipefail
DIR=$1 ROWS=$2
REPO_URL=https://huggingface.co/datasets/CohereLabs/msmarco-v2.1-embed-english-v3
API=https://huggingface.co/api/datasets/CohereLabs/msmarco-v2.1-embed-english-v3/tree/main/passages_npy
mkdir -p "$DIR/npy" "$DIR/queries"

get() {       # get PATH OUT
	[ -e "$2" ] && { echo "have $2"; return; }
	for try in 1 2 3 4 5; do
		curl -fsSL --retry 5 -C - -o "$2.part" "$REPO_URL/resolve/main/$1" && mv "$2.part" "$2" && { echo "done $2"; return; }
		echo "retry $try: $1"; sleep 10
	done
	echo "could not fetch $1"; return 1
}
export -f get; export REPO_URL

get queries_parquet/queries.parquet "$DIR/queries/queries.parquet"
# each file is a float16 matrix of 1024 columns behind a 128-byte header
files=$(curl -fsSL "$API" | python3 -c "
import json, sys
rows, need = 0, int(sys.argv[1])
for e in sorted(json.load(sys.stdin), key=lambda e: e['path']):
    print(e['path'])
    rows += (e['size'] - 128) // 2048
    if rows >= need:
        break
" "$ROWS")
echo "$files" | xargs -P 4 -I{} bash -c 'get {} "$0/npy/$(basename {})"' "$DIR"
echo "fetched $(echo "$files" | wc -l) files into $DIR/npy"
