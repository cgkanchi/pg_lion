#!/usr/bin/env python3
"""Reproducible index comparison in an exclusively owned disposable cluster."""
import argparse
import concurrent.futures
import gzip
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import tempfile
import time

from db import DB, DatabaseError, digest, nodes
from workloads import (FAMILIES, FOCUSED_FAMILIES, AFTER_MAINTENANCE, scalar_data,
                       doc_data, profile_cases, profile_indexes, measurement_matrix)

ROOT = Path(__file__).resolve().parents[2]


def command(args, **kwargs):
    return subprocess.check_output([str(x) for x in args], stderr=subprocess.STDOUT, text=True, **kwargs)


class Cluster:
    def __init__(self, prefix, output, preload=False):
        self.preload = preload
        self.prefix, self.output = Path(prefix).resolve(), output
        self.root = Path(tempfile.mkdtemp(prefix='lion-comparison-', dir='/tmp'))
        self.data, self.socket = self.root/'data', self.root/'socket'
        self.socket.mkdir(mode=0o700)
        self.started = False
        self.db = None
        self.libpq = self.prefix/'lib/libpq.so'
        self.conninfo = f"host={self.socket} port=55449 user=postgres dbname=postgres"

    def start(self, initialize=False):
        if initialize:
            command([self.prefix/'bin/initdb', '-D', self.data, '-U', 'postgres', '--no-locale', '-A', 'trust'])
            with (self.data/'postgresql.conf').open('a') as f:
                f.write(f"\nlisten_addresses=''\nunix_socket_directories='{self.socket}'\nport=55449\n"
                        "shared_buffers='512MB'\nwork_mem='64MB'\nmaintenance_work_mem='512MB'\n"
                        "max_parallel_workers_per_gather=0\njit=off\nautovacuum=off\n"
                        "checkpoint_timeout='1h'\nmax_wal_size='8GB'\ntrack_io_timing=on\n"
                        "fsync=on\nsynchronous_commit=on\nfull_page_writes=on\n")
                if self.preload:
                    # pg_lion's WAL resource manager registers only from shared_preload_libraries;
                    # with it loaded, wal_mode=auto resolves to rmgr for every lion index built here.
                    f.write("shared_preload_libraries='pg_lion'\n")
        command([self.prefix/'bin/pg_ctl', '-D', self.data, '-l', self.root/'server.log', '-w', 'start'])
        self.started = True
        self.connect()

    def connect(self):
        self.db = DB(self.libpq, self.conninfo)
        actual = Path(self.db.scalar('SHOW data_directory')).resolve()
        if actual != self.data.resolve():
            self.db.close()
            raise RuntimeError(f'Unexpected cluster {actual}; refusing SQL')
        self.db.query("LOAD 'pg_lion'; SET statement_timeout='60s'")

    def stop(self):
        if self.db:
            self.db.close()
            self.db = None
        if self.started:
            command([self.prefix/'bin/pg_ctl', '-D', self.data, '-m', 'fast', '-w', 'stop'])
            self.started = False

    def close(self, keep=False):
        self.stop()
        if (self.root/'server.log').exists():
            shutil.copyfile(self.root/'server.log', self.output/'server.log')
        if not keep:
            shutil.rmtree(self.root)


class Suite:
    def __init__(self, args):
        self.args = args
        self.output = Path(args.output).resolve()
        self.previous = None
        self.completed = set()
        resumed_skips = []
        if args.resume:
            self.previous = json.loads((self.output/'metadata.json').read_text())
            previous_args = self.previous['arguments']
            if previous_args.get('profile', 'full') != args.profile:
                raise ValueError('Cannot resume with a different benchmark profile')
            for key,value in vars(args).items():
                if key != 'resume' and key in previous_args and value != previous_args[key]:
                    raise ValueError(f'Cannot resume with changed {key}: {value} != {previous_args[key]}')
            if self.previous['commit'] != command(['git','rev-parse','HEAD'],cwd=ROOT).strip():
                raise ValueError('Cannot resume measurements against a different source commit')
            old_events=[json.loads(line) for line in (self.output/'operations.jsonl').read_text().splitlines()]
            old_samples=[json.loads(line) for line in (self.output/'samples.jsonl').read_text().splitlines()]
            from audit import expected_configurations
            expected_warm, expected_cold = expected_configurations(
                self.previous, json.loads((self.output/'queries.json').read_text()))
            self.completed={(e['suite'],e['rows'],e['family']) for e in old_events if e['kind']=='dataset_complete'}
            # Compatibility with the first run, before explicit completion checkpoints existed.
            for e in old_events:
                if e['kind']!='portfolio':
                    continue
                suite,n,family=e['suite'],e['rows'],e['family']
                variants=['roaring','roaring_bitmap'] if family=='roaring' else [family]
                warm = {k for k in expected_warm if k[0] == suite and k[1] == n and k[2] in variants}
                cold = {k for k in expected_cold if k[0] == suite and k[1] == n and k[2] in variants}
                expected = len(warm)*args.repeats + len(cold)*args.cold_repeats
                actual=sum(s['suite']==suite and s['rows']==n and s['variant'] in variants for s in old_samples)
                if actual==expected:
                    self.completed.add((suite,n,family))
                # Older runners stopped on a maintenance timeout. Retain the
                # completed read matrix and explicitly mark the dependent
                # post-maintenance checks as unmeasured, never as successes.
                failed = [op for op in old_events if op.get('suite') == suite and
                          op.get('rows') == n and op.get('family') == family and
                          op['kind'] in ['insert','indexed_update','delete','vacuum','reindex'] and
                          op.get('status') == 'error']
                before_maintenance = sum(k[3] != 'after_maintenance' for k in warm)*args.repeats + len(cold)*args.cold_repeats
                if suite == 'scalar' and failed and actual == before_maintenance:
                    self.completed.add((suite,n,family))
                    if not any(op['kind']=='skipped_configuration' and op.get('suite')==suite and
                               op.get('rows')==n and op.get('variant') in variants for op in old_events):
                        for variant in variants:
                            for case in ['eq_c200_17','and2','is_null','group_c200']:
                                resumed_skips.append(dict(kind='skipped_configuration',suite=suite,rows=n,
                                    variant=variant,phase='after_maintenance',mode='default',memory='64MB',
                                    case=case,reason=f"Prior {failed[-1]['kind']} failed; read matrix retained on resume"))
            unfinished={(s['suite'],s['rows'],'roaring' if s['variant']=='roaring_bitmap' else s['variant'])
                        for s in old_samples}-self.completed
            if unfinished:
                raise ValueError(f'Partly measured portfolios need a separate output directory: {unfinished}')
        else:
            self.output.mkdir(parents=True, exist_ok=False)
        self.cluster = Cluster(args.prefix, self.output, preload=args.preload)
        self.rng = random.Random(args.seed)
        mode='a' if args.resume else 'w'
        self.samples = (self.output/'samples.jsonl').open(mode, buffering=1)
        self.plans = gzip.open(self.output/'plans.jsonl.gz', mode+'t')
        self.events = (self.output/'operations.jsonl').open(mode, buffering=1)
        for record in resumed_skips:
            self.event(**record)
        self.errors = sum(e.get('status')=='error' for e in old_events+old_samples) if args.resume else 0
        self.sequence = max((s['sequence'] for s in old_samples),default=0) if args.resume else 0

    @property
    def db(self):
        return self.cluster.db

    def log(self, message):
        print(time.strftime('%H:%M:%S'), message, flush=True)

    def event(self, **record):
        self.events.write(json.dumps(record)+'\n')

    def settings(self, variant, mode='default', memory='64MB'):
        push = 'on' if variant == 'roaring' else 'off'
        seq = 'off' if mode == 'prefer_index' else 'on'
        self.db.query(f"SET pg_lion.enable_count_pushdown={push}; SET enable_seqscan={seq}; "
                      f"SET work_mem='{memory}'; SET max_parallel_workers_per_gather=0")

    def reference(self, cases):
        self.db.query("SET pg_lion.enable_count_pushdown=off; SET enable_seqscan=on; "
                      "SET enable_indexscan=off; SET enable_indexonlyscan=off; SET enable_bitmapscan=off")
        try:
            return {c.id: digest(self.db.query(c.sql)) for c in cases}
        finally:
            self.db.query('RESET enable_indexscan; RESET enable_indexonlyscan; RESET enable_bitmapscan')

    def timed_operation(self, label, sql, allow_error=False, **context):
        before = self.db.scalar('SELECT pg_current_wal_insert_lsn()')
        start = time.perf_counter()
        error=None
        try:
            self.db.query(sql)
        except DatabaseError as e:
            error=str(e)
        elapsed = (time.perf_counter()-start)*1000
        wal = int(self.db.scalar(f"SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(),'{before}')"))
        self.event(kind=label, status='error' if error else 'ok', error=error,
                   elapsed_ms=elapsed, wal_bytes=wal, **context)
        if error:
            self.errors+=1
            if label!='build' and not allow_error:
                raise DatabaseError(error)
        return error is None

    def indexes(self, suite, family, n):
        table = 'fact' if suite == 'scalar' else 'docs'
        specs = profile_indexes(suite, family, self.args.profile)
        failed=set()
        for rep in range(self.args.build_repeats):
            for name, sql in specs:
                if name in failed:
                    continue
                self.db.query('CHECKPOINT')
                if not self.timed_operation('build', sql, suite=suite, family=family, rows=n, repeat=rep, index=name):
                    failed.add(name)
                    self.log(f'{suite} rows={n} {family}/{name}: build failed; portfolio will be partial')
                    continue
                self.event(kind='index_size', suite=suite, family=family, rows=n, repeat=rep,
                           index=name, bytes=int(self.db.scalar(f"SELECT pg_relation_size('{name}')")))
            if rep < self.args.build_repeats-1:
                for name, _ in specs:
                    if name not in failed:
                        self.db.query(f'DROP INDEX {name}')
        self.event(kind='portfolio', suite=suite, family=family, rows=n,
                   completeness='partial' if failed else 'complete', failed_indexes=sorted(failed),
                   index_bytes=int(self.db.scalar(f"SELECT pg_indexes_size('{table}')")),
                   heap_bytes=int(self.db.scalar(f"SELECT pg_table_size('{table}')")))

    def measure(self, suite, n, variant, phase, cases, expected, modes=None, memory='64MB'):
        modes = modes or ['default', 'prefer_index']
        jobs = [(mode, case) for mode in modes for case in cases]
        self.rng.shuffle(jobs)
        valid = []
        # Verify returned rows separately; EXPLAIN's aggregate row count cannot prove correctness.
        for mode, case in jobs:
            self.settings(variant, mode, memory)
            context = dict(suite=suite, rows=n, variant=variant, phase=phase, mode=mode,
                           memory=memory, case=case.id, category=case.category)
            try:
                actual = digest(self.db.query(case.sql))
                if actual != expected[case.id]:
                    raise AssertionError(f'result mismatch: {actual} != {expected[case.id]}')
                for _ in range(self.args.warmups):
                    self.db.query(case.sql)
                valid.append((mode, case, context))
                self.event(kind='correctness', status='pass', digest=actual, **context)
            except (DatabaseError, AssertionError) as e:
                self.errors += 1
                self.event(kind='correctness', status='error', error=str(e), **context)
        # Randomized complete rounds reduce query-order/time drift within each portfolio.
        for rep in range(self.args.repeats):
            self.rng.shuffle(valid)
            for mode, case, context in valid:
                self.settings(variant, mode, memory)
                self.record_query(case, context, rep)

    def record_query(self, case, context, rep):
        self.sequence += 1
        try:
            plan = self.db.explain(case.sql)
            tree = list(nodes(plan['Plan']))
            used = sorted({x['Index Name'] for x in tree if 'Index Name' in x})
            custom = any(x.get('Custom Plan Provider') == 'LionCount' for x in tree)
            scans = sorted({x['Node Type'] for x in tree if 'Scan' in x['Node Type']})
            # Custom scan embeds its own index access; EXPLAIN need not emit Index Name.
            fallback = context['variant'] != 'seq' and not used and not custom
            sample = dict(**context, repeat=rep, sequence=self.sequence, status='ok',
                          execution_ms=plan['Execution Time'], planning_ms=plan['Planning Time'],
                          indexes=used, custom_count=custom, scans=scans, fallback=fallback,
                          shared_hit_blocks=plan['Plan'].get('Shared Hit Blocks',0),
                          shared_read_blocks=plan['Plan'].get('Shared Read Blocks',0),
                          temp_written_blocks=plan['Plan'].get('Temp Written Blocks',0),
                          lossy_heap_blocks=sum(x.get('Lossy Heap Blocks',0) for x in tree))
            self.samples.write(json.dumps(sample)+'\n')
            self.plans.write(json.dumps(dict(sequence=self.sequence, context=context, plan=plan))+'\n')
        except DatabaseError as e:
            self.errors += 1
            self.samples.write(json.dumps(dict(**context, repeat=rep, sequence=self.sequence,
                                               status='error', error=str(e)))+'\n')

    def visibility(self, suite, n, family, phase):
        table = 'fact' if suite == 'scalar' else 'docs'
        vm = self.db.query(f"SELECT * FROM pg_visibility_map_summary('{table}')", dictionaries=True)[0]
        self.event(kind='visibility', suite=suite, rows=n, family=family, phase=phase,
                   heap_pages=int(self.db.scalar(f"SELECT pg_relation_size('{table}')/8192")), **vm)

    def cold(self, suite, n, variant, cases):
        for case in cases:
            for rep in range(self.args.cold_repeats):
                self.cluster.stop()
                self.cluster.start()
                self.settings(variant)
                self.record_query(case, dict(suite=suite, rows=n, variant=variant,
                                  phase='shared_buffers_cold', mode='default', memory='64MB',
                                  case=case.id, category=case.category), rep)

    def concurrent(self, suite, n, variant, case):
        for clients in self.args.clients:
            def client(worker):
                db = DB(self.cluster.libpq, self.cluster.conninfo)
                try:
                    db.query("LOAD 'pg_lion'; SET statement_timeout='60s'; "
                             f"SET pg_lion.enable_count_pushdown={'on' if variant=='roaring' else 'off'}")
                    db.query(case.sql)
                    barrier.wait()
                    start = time.perf_counter()
                    latencies = []
                    while time.perf_counter()-start < self.args.duration:
                        t = time.perf_counter()
                        db.query(case.sql)
                        latencies.append((time.perf_counter()-t)*1000)
                    return latencies, time.perf_counter()-start
                finally:
                    db.close()
            import threading
            barrier = threading.Barrier(clients, timeout=120)
            with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as executor:
                results = list(executor.map(client, range(clients)))
            latencies = sorted(x for values, _ in results for x in values)
            elapsed = max(t for _, t in results)
            self.event(kind='concurrency', suite=suite, rows=n, variant=variant, case=case.id,
                       clients=clients, duration_seconds=elapsed, transactions=len(latencies),
                       tps=len(latencies)/elapsed, median_ms=statistics.median(latencies),
                       p95_ms=latencies[min(len(latencies)-1,int(len(latencies)*.95))],
                       driver='Python threads + synchronous libpq; includes client/result overhead')

    def run_dataset(self, suite, n):
        cases = profile_cases(suite, self.args.profile)
        by_id = {c.id: c for c in cases}
        matrix = measurement_matrix(suite, self.args.profile, self.args.maintenance)
        families = list(self.args.families if suite == 'scalar' else
                        [x for x in self.args.families if x in ['seq','gin','gist','roaring']])
        self.rng.shuffle(families)
        table = 'fact' if suite == 'scalar' else 'docs'
        data = scalar_data(n) if suite == 'scalar' else doc_data(n)
        # Identical new heap per family, including dirty-page placement and physical ordering.
        for family in families:
            if (suite,n,family) in self.completed:
                self.log(f'{suite} rows={n} family={family}: retained completed measurements')
                continue
            self.log(f'{suite} rows={n} family={family}: load/build')
            self.db.query(f'DROP TABLE IF EXISTS {table} CASCADE')
            self.timed_operation('load', f'CREATE TABLE {table} WITH (fillfactor=90) AS {data}',
                                 suite=suite, family=family, rows=n)
            self.db.query(f'VACUUM (FREEZE, ANALYZE) {table}')
            self.indexes(suite, family, n)
            variants = ['roaring','roaring_bitmap'] if family == 'roaring' else [family]
            phases = list(dict.fromkeys(config['phase'] for config in matrix
                                       if config['phase'] not in ['low_work_mem', 'after_maintenance']))
            for phase in phases:
                configs = [c for c in matrix if c['phase'] == phase or
                           (phase == 'clean' and c['phase'] == 'low_work_mem')]
                selected = [by_id[name] for name in dict.fromkeys(name for c in configs for name in c['cases'])]
                if phase == 'dirty_clustered_5pct':
                    self.db.query(f"UPDATE {table} SET payload=reverse(payload) WHERE id <= {max(1,n//20)}")
                    self.db.query(f'ANALYZE {table}')
                elif phase == 'dirty_scattered':
                    self.db.query(f"UPDATE {table} SET payload=reverse(payload) WHERE id%100=0")
                    self.db.query(f'ANALYZE {table}')
                self.visibility(suite,n,family,phase)
                expected = self.reference(selected)
                self.log(f'{suite} rows={n} family={family}: {phase}, {len(selected)} cases')
                for variant in variants:
                    for config in configs:
                        self.measure(suite,n,variant,config['phase'],
                                     [by_id[name] for name in config['cases']], expected,
                                     modes=[config['mode']], memory=config['memory'])
                    if phase == 'clean' and suite == 'scalar':
                        if self.args.cold_repeats:
                            self.cold(suite,n,variant,[c for c in cases if c.id in ['eq_c200_17','fetch_medium']])
                        if self.args.duration:
                            self.concurrent(suite,n,variant,next(c for c in cases if c.id=='and2'))
            if suite == 'scalar' and self.args.maintenance:
                self.log(f'{suite} rows={n} family={family}: maintenance')
                self.settings(family)
                operations = [('insert',f'INSERT INTO fact {scalar_data(max(1,n//100),start=n+1)}'),
                              ('indexed_update',f'UPDATE fact SET c200=(c200+1)%200 WHERE id<={max(1,n//100)}'),
                              ('delete',f'DELETE FROM fact WHERE id%100=1'),
                              ('vacuum','VACUUM (ANALYZE) fact')]
                if self.args.profile == 'full':
                    operations.append(('reindex','REINDEX TABLE fact'))
                failed_operation = None
                for label,sql in operations:
                    self.db.query('CHECKPOINT')
                    succeeded = self.timed_operation(label,sql,allow_error=True,suite=suite,rows=n,family=family)
                    self.event(kind='maintenance_size',suite=suite,rows=n,family=family,operation=label,
                               index_bytes=int(self.db.scalar("SELECT pg_indexes_size('fact')")))
                    if not succeeded:
                        failed_operation = label
                        self.log(f'{suite} rows={n} family={family}: {label} failed; dependent maintenance checks skipped')
                        break
                check = [by_id[name] for name in AFTER_MAINTENANCE]
                if failed_operation:
                    for variant in variants:
                        for case in check:
                            self.event(kind='skipped_configuration',suite=suite,rows=n,variant=variant,
                                       phase='after_maintenance',mode='default',memory='64MB',case=case.id,
                                       reason=f'{failed_operation} failed; dependent maintenance checks not measured')
                else:
                    expected = self.reference(check)
                    for variant in variants:
                        self.measure(suite,n,variant,'after_maintenance',check,expected,modes=['default'])
            self.db.query(f'DROP TABLE {table}')
            self.event(kind='dataset_complete',suite=suite,rows=n,family=family)
            self.plans.flush()

    def run(self):
        metadata = dict(arguments=vars(self.args), started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                        platform=platform.platform(), cpu_count=os.cpu_count(),
                        cpuinfo=Path('/proc/cpuinfo').read_text().split('\n\n')[0],
                        meminfo=Path('/proc/meminfo').read_text(),
                        commit=command(['git','rev-parse','HEAD'],cwd=ROOT).strip(),
                        git_diff=command(['git','diff','--stat'],cwd=ROOT),
                        pg_config=command([self.cluster.prefix/'bin/pg_config','--configure']).strip(),
                        schema='Synthetic integer equality/cardinality and text-array/full-text workloads',
                        cache='Warm process/OS cache; optional shared-buffer-cold retains OS cache',
                        clock='EXPLAIN ANALYZE TIMING OFF Execution Time; planning recorded separately')
        if self.previous:
            metadata['started_utc']=self.previous['started_utc']
            metadata['resume_history']=self.previous.get('resume_history',[])+[dict(
                resumed_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                previous_status=self.previous.get('status'),previous_error=self.previous.get('error'),
                retained_portfolios=sorted(self.completed),
                note='Fresh owned cluster; retained completed portfolios; shuffled rounds restart from the configured seed')]
        try:
            self.cluster.start(initialize=True)
            extensions = ['pg_lion', 'pg_visibility']
            if 'gin' in self.args.families:
                extensions.append('btree_gin')
            if 'gist' in self.args.families:
                extensions.append('btree_gist')
            for ext in extensions:
                self.db.query(f'CREATE EXTENSION {ext}')
            metadata['server_version'] = self.db.scalar('SELECT version()')
            metadata['settings'] = self.db.query('SELECT name,setting,unit,source FROM pg_settings ORDER BY name',dictionaries=True)
            (self.output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
            (self.output/'queries.json').write_text(json.dumps({
                'scalar':[c.record() for c in profile_cases('scalar', self.args.profile)],
                'documents':[c.record() for c in profile_cases('documents', self.args.profile)],
                'matrix':{s:measurement_matrix(s, self.args.profile, self.args.maintenance)
                          for s in ['scalar', 'documents']},
                'indexes':{s:{f:profile_indexes(s,f,self.args.profile) for f in self.args.families
                             if s == 'scalar' or f in ['seq','gin','gist','roaring']}
                           for s in ['scalar','documents']}},indent=2)+'\n')
            for n in self.args.rows:
                self.run_dataset('scalar',n)
            if self.args.documents:
                self.run_dataset('documents',self.args.documents)
            metadata['completed_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
            metadata['errors'] = self.errors
            metadata['status'] = 'complete' if not self.errors else 'complete_with_errors'
        except BaseException as e:
            metadata['status'], metadata['error'] = 'incomplete',repr(e)
            raise
        finally:
            (self.output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
            self.samples.close()
            self.plans.close()
            self.events.close()
            self.cluster.close(keep=self.args.keep_cluster)
            self.log(f"artifacts: {self.output}; recorded errors: {self.errors}")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prefix',required=True,help='Installed PostgreSQL with pg_lion, pg_visibility and support extensions for selected families')
    p.add_argument('--output',required=True,help='New output directory (refuses overwrite)')
    p.add_argument('--profile', choices=['focused', 'full'], default='focused',
                   help='focused (default): relevant competitors and representative cases; full: legacy exhaustive matrix')
    p.add_argument('--rows',type=int,nargs='+',default=[1000000,5000000])
    p.add_argument('--documents',type=int,default=200000)
    p.add_argument('--families',nargs='+',choices=FAMILIES,help='Default: btree gin roaring; full profile: all families. roaring also measures roaring_bitmap')
    p.add_argument('--repeats',type=int,help='Timing rounds: focused 3, full 10')
    p.add_argument('--warmups',type=int,help='Warmups: focused 1, full 2')
    p.add_argument('--build-repeats',type=int,help='Build trials: focused 1, full 3')
    p.add_argument('--cold-repeats',type=int,help='Restart trials per selected query: focused 0, full 3')
    p.add_argument('--clients',type=int,nargs='+',default=[1,4,8])
    p.add_argument('--duration',type=float,help='Seconds per concurrency point: focused 0 (disabled), full 5')
    p.add_argument('--maintenance',action=argparse.BooleanOptionalAction,default=True)
    p.add_argument('--keep-cluster',action='store_true')
    p.add_argument('--resume',action='store_true',help='Resume a stopped run at a completed portfolio boundary, with identical arguments')
    p.add_argument('--seed',type=int,default=20260920)
    p.add_argument('--preload',action='store_true',help="Start the cluster with shared_preload_libraries='pg_lion' so lion indexes use the custom WAL resource manager (wal_mode=auto -> rmgr)")
    args = p.parse_args(argv)
    full = args.profile == 'full'
    defaults = dict(families=FAMILIES if full else FOCUSED_FAMILIES,
                    repeats=10 if full else 3, warmups=2 if full else 1,
                    build_repeats=3 if full else 1, cold_repeats=3 if full else 0,
                    duration=5 if full else 0)
    for name, value in defaults.items():
        if getattr(args, name) is None:
            setattr(args, name, value)
    if min(args.rows)<1 or args.documents<0 or args.repeats<1 or args.build_repeats<1 or args.warmups<0 or args.cold_repeats<0 or args.duration<0 or min(args.clients)<1:
        p.error('Rows, repeats, builds, and clients must be positive; optional counts must be nonnegative')
    for name in ('rows', 'families', 'clients'):
        if len(getattr(args, name)) != len(set(getattr(args, name))):
            p.error(f'--{name} must not contain duplicates')
    return args


if __name__=='__main__':
    Suite(parse_args()).run()
