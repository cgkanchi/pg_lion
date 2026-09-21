#!/usr/bin/env python3
"""Owned-cluster comparison of multikey full-index fallback costing."""
import argparse
import json
from pathlib import Path
import random
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'bench/comprehensive'))
from db import digest
from run import Cluster
from workloads import doc_data

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--prefix', required=True)
p.add_argument('--output', required=True)
p.add_argument('--rows', type=int, default=200000)
p.add_argument('--repeats', type=int, default=10)
args = p.parse_args()
output = Path(args.output).resolve()
output.mkdir(parents=True, exist_ok=False)
cluster = Cluster(args.prefix, output)
samples = []
metadata = {'arguments': vars(args)}
queries = {'phrase': "SELECT count(*) FROM docs WHERE tsv @@ 'common <-> w1'::tsquery",
           'prefix': "SELECT count(*) FROM docs WHERE tsv @@ 'rare12:*'::tsquery"}

def settings(db, mode):
    enabled = 'on' if mode == 'default' else 'off'
    db.query(f'SET enable_indexscan={enabled}; SET enable_indexonlyscan={enabled}; '
             f'SET enable_bitmapscan={enabled}; SET enable_seqscan=on')

try:
    cluster.start(initialize=True)
    db = cluster.db
    db.query('CREATE EXTENSION roaring_index')
    db.query('CREATE TABLE docs WITH (fillfactor=90) AS ' + doc_data(args.rows))
    db.query('VACUUM (FREEZE,ANALYZE) docs')
    db.query('CREATE INDEX ix_tsv ON docs USING roaring(tsv)')
    metadata['server'] = db.scalar('SELECT version()')
    metadata['heap_pages'] = db.scalar("SELECT pg_relation_size('docs')/8192")
    settings(db, 'sequential')
    expected = {name: digest(db.query(sql)) for name, sql in queries.items()}
    for mode in ['default', 'sequential']:
        settings(db, mode)
        for name, sql in queries.items():
            if digest(db.query(sql)) != expected[name]:
                raise AssertionError(f'{name}/{mode} disagrees with sequential result')
            db.query(sql)
    rng = random.Random(20260921)
    for rep in range(args.repeats):
        jobs = [(name, mode) for name in queries for mode in ['default', 'sequential']]
        rng.shuffle(jobs)
        for name, mode in jobs:
            settings(db, mode)
            samples.append({'query': name, 'mode': mode, 'repeat': rep, 'plan': db.explain(queries[name])})
    metadata['correctness'] = 'both routes match the exact sequential result for both queries'
    metadata['medians_ms'] = {name: {mode: statistics.median(
        s['plan']['Execution Time'] for s in samples if s['query'] == name and s['mode'] == mode)
        for mode in ['default', 'sequential']} for name in queries}
    print(json.dumps(metadata, indent=2))
finally:
    (output / 'results.json').write_text(json.dumps({'metadata': metadata, 'queries': queries,
                                                   'samples': samples}, indent=2) + '\n')
    cluster.close()
