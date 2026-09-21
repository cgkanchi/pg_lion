#!/usr/bin/env python3

def am_name(family):
    """Access method name for a benchmark family label (the roaring family's AM is 'lion')."""
    return 'lion' if family == 'roaring' else family
"""Supplement: incremental growth, changing-key churn, dirty counts, hot-key writers."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import random
import statistics
import threading
import time

from db import DB, digest
from run import Cluster, ROOT, command

METHODS=['seq','btree','hash','gin','gist','brin','roaring']


class Stress:
    def __init__(self,args):
        self.args=args
        self.output=Path(args.output).resolve()
        self.output.mkdir(parents=True,exist_ok=False)
        self.cluster=Cluster(args.prefix,self.output)
        self.records=(self.output/'stress.jsonl').open('w',buffering=1)
        self.rng=random.Random(args.seed)
        self.metadata=dict(arguments=vars(args),commit=command(['git','rev-parse','HEAD'],cwd=ROOT).strip(),
                           started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))

    @property
    def db(self):
        return self.cluster.db

    def record(self,**values):
        self.records.write(json.dumps(values)+'\n')

    def timed(self,sql,**context):
        before=self.db.scalar('SELECT pg_current_wal_insert_lsn()')
        t=time.perf_counter()
        self.db.query(sql)
        elapsed=(time.perf_counter()-t)*1000
        wal=int(self.db.scalar(f"SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(),'{before}')"))
        self.record(**context,elapsed_ms=elapsed,wal_bytes=wal)

    def indexes(self,method,table='fact',columns=('k','h')):
        if method!='seq':
            for c in columns:
                suffix=' WITH (pages_per_range=32)' if method=='brin' else ''
                self.db.query(f'CREATE INDEX ON {table} USING {am_name(method)} ({c}){suffix}')

    def reference(self,sql):
        self.db.query('SET pg_lion.enable_count_pushdown=off; SET enable_indexscan=off; '
                      'SET enable_indexonlyscan=off; SET enable_bitmapscan=off; SET enable_seqscan=on')
        try:
            return digest(self.db.query(sql))
        finally:
            self.db.query('RESET enable_indexscan; RESET enable_indexonlyscan; RESET enable_bitmapscan')

    def queries(self,method,profile,queries,**context):
        variants=['roaring','roaring_bitmap'] if method=='roaring' else [method]
        if method=='roaring' and profile=='dirty_memory':
            variants.append('roaring_sql')
        expected={name:self.reference(sql) for name,sql in queries}
        for variant in variants:
            self.db.query(f"SET pg_lion.enable_count_pushdown={'on' if variant=='roaring' else 'off'}; "
                          f"SET enable_seqscan={'off' if profile=='dirty_memory' else 'on'}")
            actual_queries=queries
            if variant=='roaring_sql':
                actual_queries=[('dense',"SELECT lion_index_count('fact_h_idx',0::int)"),
                                ('group',"SELECT 0,lion_index_count('fact_h_idx',0::int) UNION ALL SELECT 1,lion_index_count('fact_h_idx',1::int)")]
                self.record(kind='recheck_stats',profile=profile,variant=variant,**context,
                            **self.db.query("SELECT * FROM lion_index_count_stats('fact_h_idx',0::int)",dictionaries=True)[0])
            for name,sql in actual_queries:
                actual=digest(self.db.query(sql))
                if actual!=expected[name]:
                    raise AssertionError(f'{profile}/{variant}/{name}: result mismatch')
                self.record(kind='correctness',profile=profile,variant=variant,query=name,status='pass',**context)
                self.db.query(sql)
            for rep in range(self.args.repeats):
                order=list(actual_queries)
                self.rng.shuffle(order)
                for name,sql in order:
                    plan=self.db.explain(sql)
                    self.record(kind='query',profile=profile,variant=variant,query=name,repeat=rep,
                                sql=sql,execution_ms=plan['Execution Time'],plan=plan,**context)

    def size(self,method,profile,**context):
        self.record(kind='size',profile=profile,method=method,
                    index_bytes=int(self.db.scalar("SELECT pg_indexes_size('fact')")),
                    heap_bytes=int(self.db.scalar("SELECT pg_table_size('fact')")),**context)
        if method=='roaring':
            for column in ['k','h']:
                stats=self.db.query(f"SELECT * FROM lion_index_stats('fact_{column}_idx')",dictionaries=True)[0]
                self.record(kind='roaring_stats',profile=profile,method=method,index=f'fact_{column}_idx',**context,**stats)

    def growth(self,method):
        n=self.args.rows
        query=f"SELECT i::bigint AS k,(i%2)::int AS h,repeat(md5(i::text),2) AS payload FROM generate_series(1,{n}) i"
        for order in ['bulk_then_index','index_then_insert']:
            print(time.strftime('%H:%M:%S'),method,order,flush=True)
            self.db.query('CREATE TABLE fact (k bigint,h int,payload text) WITH (fillfactor=90)')
            if order=='index_then_insert':
                self.indexes(method)
            self.db.query('CHECKPOINT')
            self.timed('INSERT INTO fact '+query,kind='load',profile='growth',method=method,order=order)
            if order=='bulk_then_index':
                t=time.perf_counter()
                self.indexes(method)
                self.record(kind='build',profile='growth',method=method,order=order,elapsed_ms=(time.perf_counter()-t)*1000)
            self.db.query('VACUUM (FREEZE,ANALYZE) fact')
            self.size(method,'growth',order=order)
            self.queries(method,'growth',[
                ('point',f'SELECT count(*) FROM fact WHERE k={n//2}::bigint'),
                ('dense','SELECT count(*) FROM fact WHERE h=0'),
                ('in_100','SELECT count(*) FROM fact WHERE k IN ('+','.join(str(1+i*n//100) for i in range(100))+')')],order=order)
            if order=='bulk_then_index':
                self.prepared(method)
                # Clear every populated heap page's VM bit and exceed the 64kB recheck-TID budget.
                self.db.query('UPDATE fact SET payload=reverse(payload); ANALYZE fact')
                self.record(kind='visibility',profile='dirty_memory',method=method,
                            **self.db.query("SELECT * FROM pg_visibility_map_summary('fact')",dictionaries=True)[0])
                for memory in ['64kB','4MB','64MB']:
                    self.db.query(f"SET work_mem='{memory}'")
                    self.queries(method,'dirty_memory',[
                        ('dense','SELECT count(*) FROM fact WHERE h=0'),
                        ('group','SELECT h,count(*) FROM fact GROUP BY h')],memory=memory)
                self.db.query("SET work_mem='64MB'")
            self.db.query('DROP TABLE fact')

    def prepared(self,method):
        point=self.args.rows//2
        queries=[('dense','EXECUTE bench_dense(0)','SELECT count(*) FROM fact WHERE h=0'),
                 ('point',f'EXECUTE bench_point({point})',f'SELECT count(*) FROM fact WHERE k={point}::bigint')]
        expected={name:self.reference(sql) for name,_,sql in queries}
        for mode in ['force_custom_plan','force_generic_plan']:
            self.db.query(f"SET pg_lion.enable_count_pushdown={'on' if method=='roaring' else 'off'}; "
                          f"SET enable_seqscan=on; SET plan_cache_mode={mode}; "
                          'PREPARE bench_dense(int) AS SELECT count(*) FROM fact WHERE h=$1; '
                          'PREPARE bench_point(bigint) AS SELECT count(*) FROM fact WHERE k=$1')
            for name,sql,_ in queries:
                if digest(self.db.query(sql))!=expected[name]:
                    raise AssertionError(f'Prepared result mismatch: {method}/{mode}/{name}')
                self.record(kind='correctness',profile='prepared',variant=method,plan_mode=mode,query=name,status='pass')
                self.db.query(sql)
            for rep in range(self.args.repeats):
                order=list(queries)
                self.rng.shuffle(order)
                for name,sql,_ in order:
                    plan=self.db.explain(sql)
                    self.record(kind='query',profile='prepared',variant=method,query=name,plan_mode=mode,
                                sql=sql,repeat=rep,execution_ms=plan['Execution Time'],plan=plan)
            self.db.query('DEALLOCATE bench_dense; DEALLOCATE bench_point')
        self.db.query('RESET plan_cache_mode')

    def churn(self,method):
        n=self.args.rows
        print(time.strftime('%H:%M:%S'),method,'churn',flush=True)
        self.db.query(f'CREATE TABLE fact AS SELECT i::bigint AS k,(i%2)::int AS h FROM generate_series(1,{n}) i')
        self.indexes(method)
        self.db.query('VACUUM (FREEZE,ANALYZE) fact')
        self.size(method,'churn',cycle=0)
        for cycle in range(1,self.args.cycles+1):
            self.db.query('CHECKPOINT')
            self.timed(f'UPDATE fact SET k=k+{n}',kind='update',profile='churn',method=method,cycle=cycle)
            self.timed('VACUUM (ANALYZE) fact',kind='vacuum',profile='churn',method=method,cycle=cycle)
            self.size(method,'churn',cycle=cycle)
            self.queries(method,'churn',[
                ('point',f'SELECT count(*) FROM fact WHERE k={cycle*n+n//2}::bigint'),
                ('obsolete',f'SELECT count(*) FROM fact WHERE k={n//2}::bigint'),
                ('dense','SELECT count(*) FROM fact WHERE h=0')],cycle=cycle)
        self.timed('REINDEX TABLE fact',kind='reindex',profile='churn',method=method,cycle=self.args.cycles)
        self.size(method,'after_reindex',cycle=self.args.cycles)
        self.db.query('DROP TABLE fact')

    def writers(self,method):
        for clients in self.args.clients:
            print(time.strftime('%H:%M:%S'),method,'writers',clients,flush=True)
            self.db.query('CREATE TABLE writes (k bigint,h int)')
            self.indexes(method,table='writes',columns=('h',))
            self.db.query('CHECKPOINT')
            before=self.db.scalar('SELECT pg_current_wal_insert_lsn()')
            barrier=threading.Barrier(clients,timeout=120)
            def writer(worker):
                db=DB(self.cluster.libpq,self.cluster.conninfo)
                try:
                    db.query("LOAD 'pg_lion'; SET statement_timeout='60s'")
                    barrier.wait()
                    start=time.perf_counter()
                    batch=0
                    latency=[]
                    while time.perf_counter()-start<self.args.duration:
                        t=time.perf_counter()
                        db.query(f'INSERT INTO writes SELECT {worker*10000000000+batch*100}::bigint+i,i%2 FROM generate_series(1,100) i')
                        latency.append((time.perf_counter()-t)*1000)
                        batch+=1
                    return latency,time.perf_counter()-start
                finally:
                    db.close()
            with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
                result=list(pool.map(writer,range(clients)))
            latency=sorted(x for values,_ in result for x in values)
            elapsed=max(t for _,t in result)
            expected=100*len(latency)
            actual=int(self.db.scalar('SELECT count(*) FROM writes'))
            if actual!=expected:
                raise AssertionError(f'Writer row count {actual} != {expected}')
            wal=int(self.db.scalar(f"SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(),'{before}')"))
            self.record(kind='writers',profile='writers',method=method,clients=clients,transactions=len(latency),
                        rows=actual,duration_seconds=elapsed,tps=len(latency)/elapsed,
                        median_ms=statistics.median(latency),p95_ms=latency[min(len(latency)-1,int(len(latency)*.95))],
                        wal_bytes=wal,index_bytes=int(self.db.scalar("SELECT pg_indexes_size('writes')")),
                        correctness='row_count_pass')
            self.db.query('SET enable_seqscan=off; SET pg_lion.enable_count_pushdown=off')
            for key in [0,1]:
                sql=f'SELECT count(*) FROM writes WHERE h={key}'
                indexed=int(self.db.scalar(sql))
                if indexed!=expected//2:
                    raise AssertionError(f'Concurrent index mismatch: {method}/{clients}/{key}: {indexed} != {expected//2}')
                self.record(kind='writer_index_check',profile='writers',method=method,clients=clients,
                            key=key,expected=expected//2,actual=indexed,status='pass',plan=self.db.explain(sql,analyze=False))
            self.db.query('RESET enable_seqscan')
            # GIN's pending-list work is deferred, so record its drain cost after the burst.
            self.timed('VACUUM (ANALYZE) writes',kind='writer_vacuum',profile='writers',method=method,clients=clients)
            self.db.query('DROP TABLE writes')

    def run(self):
        try:
            self.cluster.start(initialize=True)
            for ext in ['pg_lion','btree_gin','btree_gist','pg_visibility']:
                self.db.query(f'CREATE EXTENSION {ext}')
            self.metadata['server_version']=self.db.scalar('SELECT version()')
            self.metadata['settings']=self.db.query('SELECT name,setting,unit FROM pg_settings ORDER BY name',dictionaries=True)
            methods=list(METHODS)
            self.rng.shuffle(methods)
            for method in methods:
                self.growth(method)
                self.churn(method)
                if self.args.duration:
                    self.writers(method)
            self.metadata['status']='complete'
        except BaseException as error:
            self.metadata['status']='incomplete'
            self.metadata['error']=repr(error)
            raise
        finally:
            self.metadata['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
            (self.output/'metadata.json').write_text(json.dumps(self.metadata,indent=2)+'\n')
            self.records.close()
            self.cluster.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix',required=True)
    p.add_argument('--output',required=True)
    p.add_argument('--rows',type=int,default=100000)
    p.add_argument('--cycles',type=int,default=5)
    p.add_argument('--repeats',type=int,default=10)
    p.add_argument('--clients',type=int,nargs='+',default=[1,4,8])
    p.add_argument('--duration',type=float,default=3)
    p.add_argument('--seed',type=int,default=20260921)
    args=p.parse_args()
    if args.rows<100 or args.cycles<1 or args.repeats<1 or args.duration<0 or min(args.clients)<1:
        p.error('Rows >= 100, cycles/repeats/clients >= 1, duration >= 0 required')
    Stress(args).run()
