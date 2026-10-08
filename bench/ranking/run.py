#!/usr/bin/env python3
"""Top-10 ranked full-text search: lion's BM25 scan against GIN + ts_rank.

Synthetic corpus: documents of 20-200 words drawn from a 50,000-word vocabulary
with a roughly Zipfian (log-uniform) frequency, so some words are in most
documents and most words are rare.  Both indexes are built over the same table.
Each query is timed with EXPLAIN ANALYZE (execution time), median of --repeats
runs after one warmup.

    python3 bench/ranking/run.py --dsn 'host=/tmp port=5499 user=postgres dbname=rank' --docs 500000
"""
import argparse, json, re, statistics, subprocess, sys

SETUP = """
DROP TABLE IF EXISTS rdocs;
SELECT setseed(0.42);
CREATE TABLE rdocs AS
SELECT i AS id,
       to_tsvector('simple', (SELECT string_agg('w' || floor(exp(random() * ln(50000)))::int, ' ')
                                FROM generate_series(1, 20 + (random() * 180)::int) g
                               WHERE g > 0 * i)) AS tsv
  FROM generate_series(1, {docs}) i;
CREATE INDEX rdocs_gin ON rdocs USING gin (tsv);
CREATE INDEX rdocs_lion ON rdocs USING lion (tsv) WITH (store_positions = true);
VACUUM (FREEZE, ANALYZE) rdocs;
"""

# (label, tsquery): word ids are ranks, w1 the most common
QUERIES = [
    ('two rare words', 'w20000 | w31000'),
    ('one common, two rare words', 'w3 | w20000 | w31000'),
    ('two mid-frequency words', 'w300 | w700'),
    ('three common words', 'w2 | w5 | w9'),
    ('rare AND', 'w2000 & w3000'),
]

LION = """SELECT id, lion_bm25_score(tsv, '{q}', 'rdocs_lion') AS s FROM rdocs
 WHERE tsv @@ '{q}' ORDER BY lion_bm25_score(tsv, '{q}', 'rdocs_lion') DESC LIMIT 10"""
GIN = """SELECT id, ts_rank(tsv, '{q}') AS s FROM rdocs
 WHERE tsv @@ '{q}' ORDER BY ts_rank(tsv, '{q}') DESC LIMIT 10"""
GIN_GUCS = "SET pg_lion.enable_bm25_scan = off; SET enable_seqscan = off; SET enable_indexscan = off;"
LION_GUCS = "SET enable_seqscan = off;"


def psql(dsn, sql):
    # from stdin, so that each statement is its own transaction (VACUUM needs that)
    return subprocess.check_output(['psql', dsn, '-XqAt', '-v', 'ON_ERROR_STOP=1'], input=sql, text=True)


def timed(dsn, gucs, sql, repeats):
    out = []
    for _ in range(repeats + 1):
        plan = psql(dsn, gucs + ' EXPLAIN (ANALYZE, COSTS OFF) ' + sql)
        out.append(float(re.search(r'Execution Time: ([\d.]+)', plan).group(1)))
    return statistics.median(out[1:]), plan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dsn', required=True)
    ap.add_argument('--docs', type=int, default=500000)
    ap.add_argument('--repeats', type=int, default=5)
    ap.add_argument('--skip-setup', action='store_true')
    a = ap.parse_args()
    if not a.skip_setup:
        psql(a.dsn, SETUP.format(docs=a.docs))
    rows = []
    for label, q in QUERIES:
        matches = int(psql(a.dsn, f"SELECT count(*) FROM rdocs WHERE tsv @@ '{q}'"))
        gin, _ = timed(a.dsn, GIN_GUCS, GIN.format(q=q), a.repeats)
        lion, plan = timed(a.dsn, LION_GUCS, LION.format(q=q), a.repeats)
        rows.append(dict(query=label, tsquery=q, matches=matches, gin_ms=gin, lion_ms=lion,
                         lion_scan='LionBm25' in plan))
        print(json.dumps(rows[-1]), flush=True)
    print('\n| Query | Matches | GIN + ts_rank ms | Lion BM25 ms | Lion speedup |')
    print('| --- | --- | --- | --- | --- |')
    for r in rows:
        print(f"| {r['query']} | {r['matches']:,} | {r['gin_ms']:.1f} | {r['lion_ms']:.1f} | {r['gin_ms']/r['lion_ms']:.1f}x |")


if __name__ == '__main__':
    main()
