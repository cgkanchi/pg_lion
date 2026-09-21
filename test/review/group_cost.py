#!/usr/bin/env python3
"""Focused, owned-cluster reproduction of grouped-count cache costing."""
import argparse
import json
from pathlib import Path
import random
import statistics
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'bench/comprehensive'))
from db import digest
from run import Cluster
from workloads import scalar_data

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--prefix',required=True)
p.add_argument('--output',required=True,help='New artifact directory')
p.add_argument('--rows',type=int,default=5000000)
p.add_argument('--repeats',type=int,default=10)
args=p.parse_args()
if args.rows<1000 or args.repeats<1:
    p.error('rows >= 1000 and repeats >= 1 required')
output=Path(args.output).resolve()
output.mkdir(parents=True,exist_ok=False)
cluster=Cluster(args.prefix,output)
results=[]
metadata={}
try:
    cluster.start(initialize=True)
    db=cluster.db
    db.query('CREATE EXTENSION roaring_index; CREATE EXTENSION pg_visibility')
    db.query('CREATE TABLE fact WITH (fillfactor=90) AS '+scalar_data(args.rows))
    db.query('VACUUM (FREEZE,ANALYZE) fact')
    db.query('CREATE INDEX ix_c200 ON fact USING roaring(c200)')
    db.query(f'UPDATE fact SET payload=reverse(payload) WHERE id<={args.rows//20}; ANALYZE fact')
    metadata={'arguments':vars(args),'server':db.scalar('SELECT version()'),
              'visibility':db.query("SELECT * FROM pg_visibility_map_summary('fact')",dictionaries=True)[0],
              'heap_pages':db.scalar("SELECT pg_relation_size('fact')/8192")}
    sql='SELECT c200,count(*) FROM fact GROUP BY c200'
    db.query('SET roaring_index.enable_count_pushdown=off')
    expected=digest(db.query(sql))
    db.query('SET roaring_index.enable_count_pushdown=on')
    for setting in ['on','off']:
        db.query('SET enable_seqscan='+setting)
        if digest(db.query(sql))!=expected:
            raise AssertionError('Forced and default plans disagree with the sequential reference')
        db.query(sql)
    rng=random.Random(20260921)
    for rep in range(args.repeats):
        order=['on','off']
        rng.shuffle(order)
        for setting in order:
            db.query('SET enable_seqscan='+setting)
            plan=db.explain(sql)
            results.append({'repeat':rep,'enable_seqscan':setting,'plan':plan})
    metadata['medians_ms']={setting:statistics.median(r['plan']['Execution Time'] for r in results if r['enable_seqscan']==setting)
                            for setting in ['on','off']}
    metadata['correctness']='both plans match the sequential result'
    print(json.dumps(metadata,indent=2))
finally:
    (output/'results.json').write_text(json.dumps({'metadata':metadata,'samples':results},indent=2)+'\n')
    cluster.close()
