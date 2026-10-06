#!/usr/bin/env python3
"""Run the small, correctness-checked progress benchmark and optional baseline comparison."""
import argparse
from collections import defaultdict
import csv
import gzip
import hashlib
import html
import json
import os
from pathlib import Path
import platform
import signal
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / 'comprehensive'))
from run import ROOT, Suite, command
from workloads import scalar_cases, doc_cases, scalar_data, doc_data, index_specs

PROFILE = 'quick-v3'
FAMILIES = ['btree', 'gin', 'roaring']
CLEAN = ['eq_c2_0', 'eq_c200_17', 'eq_c20k_123', 'in_c20k_100', 'and2', 'and3', 'and3_selective',
         'is_null', 'fetch_medium', 'range_random', 'group_c200']
DIRTY = ['eq_c2_0', 'eq_c200_17', 'and2', 'group_c200']
AFTER = ['eq_c200_17', 'group_c200']
DOCS = ['array_common', 'array_and', 'array_or', 'ts_common', 'ts_and', 'ts_phrase', 'ts_prefix', 'ts_fetch']
KEY = ['suite', 'rows', 'variant', 'phase', 'mode', 'memory', 'case']
SETTINGS = ['shared_buffers', 'work_mem', 'maintenance_work_mem', 'max_parallel_workers_per_gather',
            'max_parallel_maintenance_workers', 'jit', 'fsync', 'synchronous_commit', 'full_page_writes',
            'autovacuum', 'random_page_cost', 'seq_page_cost', 'effective_cache_size', 'hash_mem_multiplier']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def cases(suite, names):
    available = {c.id: c for c in (scalar_cases() if suite == 'scalar' else doc_cases())}
    return [available[name] for name in names]


def phases(suite):
    if suite == 'documents':
        return [('clean', DOCS, 'default', '64MB')]
    return [('clean', CLEAN, 'default', '64MB'),
            ('dirty_scattered', DIRTY, 'default', '64MB'),
            ('dirty_scattered', ['group_c200'], 'prefer_index', '64MB'),
            ('after_maintenance', AFTER, 'default', '64MB')]


def quick_indexes(suite, family):
    # Keep the full-width heap, but build only indexes exercised by this profile.
    names = {'ix_c2', 'ix_c20', 'ix_c200', 'ix_c20k', 'ix_nullable'} if suite == 'scalar' else {'ix_tags', 'ix_tsv'}
    return [(name, sql) for name, sql in index_specs(suite, family) if name in names]


def workload(args):
    configs = []
    datasets = [('scalar', n) for n in args.rows]
    if args.documents:
        datasets.append(('documents', args.documents))
    for suite, n in datasets:
        for family in FAMILIES if suite == 'scalar' else ['gin', 'roaring']:
            for variant in ['roaring', 'roaring_bitmap'] if family == 'roaring' else [family]:
                for phase, names, mode, memory in phases(suite):
                    configs.extend(dict(zip(KEY, [suite, n, variant, phase, mode, memory, name])) for name in names)
    return dict(configurations=configs,
                scalar=[c.record() for c in cases('scalar', list(dict.fromkeys(CLEAN + DIRTY + AFTER)))],
                documents=[c.record() for c in cases('documents', DOCS)],
                data_sql={'scalar': {str(n): scalar_data(n) for n in args.rows}, 'documents': doc_data(args.documents)},
                indexes={s: {f: quick_indexes(s, f) for f in (FAMILIES if s == 'scalar' else ['gin', 'roaring'])}
                         for s in ['scalar', 'documents']})


class QuickSuite(Suite):
    def indexes(self, suite, family, n):
        table = 'fact' if suite == 'scalar' else 'docs'
        for name, sql in quick_indexes(suite, family):
            self.db.query('CHECKPOINT')
            if not self.timed_operation('build', sql, suite=suite, family=family, rows=n, repeat=0, index=name):
                raise RuntimeError(f'Index {name} failed; refusing a partial quick baseline')
            self.event(kind='index_size', suite=suite, family=family, rows=n, repeat=0,
                       index=name, bytes=int(self.db.scalar(f"SELECT pg_relation_size('{name}')")))
        self.event(kind='portfolio', suite=suite, family=family, rows=n, completeness='complete', failed_indexes=[],
                   index_bytes=int(self.db.scalar(f"SELECT pg_indexes_size('{table}')")),
                   heap_bytes=int(self.db.scalar(f"SELECT pg_table_size('{table}')")))

    def close_owned(self):
        # A deadline can interrupt pg_ctl after it launched the owned postmaster
        # but before Cluster.start sets its flag. Never remove live server data.
        if (self.cluster.data/'postmaster.pid').exists():
            self.cluster.started = True
        self.cluster.close()

    def run(self):
        try:
            self.run_owned()
        finally:
            # Also clean up if provenance collection fails before server startup.
            if not self.samples.closed:
                self.samples.close()
                self.plans.close()
                self.events.close()
                self.close_owned()

    def run_dataset(self, suite, n):
        table = 'fact' if suite == 'scalar' else 'docs'
        families = list(FAMILIES if suite == 'scalar' else ['gin', 'roaring'])
        self.rng.shuffle(families)
        for family in families:
            self.log(f'{suite} {n:,} rows: {family}')
            self.db.query(f'CREATE TABLE {table} WITH (fillfactor=90) AS ' +
                          (scalar_data(n) if suite == 'scalar' else doc_data(n)))
            self.db.query(f'VACUUM (FREEZE,ANALYZE) {table}')
            self.indexes(suite, family, n)
            if self.errors:
                raise RuntimeError('Index build failed; refusing a partial quick baseline')
            variants = ['roaring', 'roaring_bitmap'] if family == 'roaring' else [family]
            last_phase = None
            for phase, names, mode, memory in phases(suite):
                if phase != last_phase:
                    if phase == 'dirty_scattered':
                        self.db.query(f'UPDATE {table} SET payload=reverse(payload) WHERE id%100=0')
                        self.db.query(f'ANALYZE {table}')
                    elif phase == 'after_maintenance':
                        self.settings(family)
                        for label, sql in [
                            ('insert', 'INSERT INTO fact ' + scalar_data(max(1, n // 100), start=n + 1)),
                            ('indexed_update', f'UPDATE fact SET c200=(c200+1)%200 WHERE id<={max(1,n//100)}'),
                            ('vacuum', 'VACUUM (ANALYZE) fact'),
                            # Mid-chain inserts: free every hundredth row's slot across the whole
                            # heap, let VACUUM hand those pages back, then insert rows whose TIDs
                            # land in them (container keys in the middle of every posting set).
                            ('delete', 'DELETE FROM fact WHERE id%100=1'),
                            ('vacuum_after_delete', 'VACUUM (ANALYZE) fact'),
                            ('mid_insert', 'INSERT INTO fact ' + scalar_data(max(1, n // 100), start=2 * n + 1)),
                        ]:
                            self.db.query('CHECKPOINT')
                            self.timed_operation(label, sql, suite=suite, rows=n, family=family)
                            self.event(kind='maintenance_size', suite=suite, rows=n, family=family,
                                       operation=label, index_bytes=int(self.db.scalar("SELECT pg_indexes_size('fact')")))
                    self.visibility(suite, n, family, phase)
                selected = cases(suite, names)
                expected = self.reference(selected)
                for variant in variants:
                    self.measure(suite, n, variant, phase, selected, expected, modes=[mode], memory=memory)
                    if self.errors:
                        raise RuntimeError('Query or correctness check failed; refusing an invalid quick baseline')
                last_phase = phase
            self.db.query(f'DROP TABLE {table}')
            if suite == 'scalar' and n == min(self.args.rows) and family in ('btree', 'roaring'):
                self.churn(family)
            self.event(kind='dataset_complete', suite=suite, rows=n, family=family)

    def churn(self, family, rows=100000, cycles=2):
        """Changing-key churn at fixed live cardinality: every unique key is rewritten, then VACUUM.
        Records per-cycle time and WAL as operations and the index size after each cycle; for lion
        also the entry count (it must stay at one per live row once VACUUM deletes empty entries)."""
        method = 'lion' if family == 'roaring' else family
        self.db.query(f'CREATE TABLE churn AS SELECT i AS id, i::bigint*7919 AS k FROM generate_series(1,{rows}) i')
        self.db.query(f'CREATE INDEX churn_k ON churn USING {method} (k)')
        self.db.query('VACUUM (FREEZE, ANALYZE) churn')
        self.settings(family)
        for cycle in range(1, cycles + 1):
            self.db.query('CHECKPOINT')
            self.timed_operation('churn_update', f'UPDATE churn SET k = k + {rows * 7919}',
                                 suite='churn', rows=rows, family=family, cycle=cycle)
            self.timed_operation('churn_vacuum', 'VACUUM (ANALYZE) churn',
                                 suite='churn', rows=rows, family=family, cycle=cycle)
            extra = {}
            if family == 'roaring':
                extra['entries'] = int(self.db.scalar("SELECT entries FROM lion_index_stats('churn_k')"))
            self.event(kind='churn_size', suite='churn', rows=rows, family=family, cycle=cycle,
                       index_bytes=int(self.db.scalar("SELECT pg_relation_size('churn_k')")), **extra)
        self.db.query('DROP TABLE churn')

    def run_owned(self):
        started = time.monotonic()
        spec = workload(self.args)
        prefix = self.cluster.prefix
        meta = dict(profile=PROFILE, arguments=vars(self.args), status='running',
                    started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                    commit=command(['git', 'rev-parse', 'HEAD'], cwd=ROOT).strip(),
                    source_diff=command(['git', 'diff', '--', 'src', '*.sql', 'Makefile'], cwd=ROOT),
                    platform=platform.platform(), cpu_count=os.cpu_count(),
                    cpu_model=next(x.split(':', 1)[1].strip() for x in Path('/proc/cpuinfo').read_text().splitlines()
                                   if x.startswith('model name')),
                    pg_config=command([prefix/'bin/pg_config', '--configure']).strip(),
                    postgres_sha256=sha(prefix/'bin/postgres'),
                    extension_sha256=sha(Path(command([prefix/'bin/pg_config', '--pkglibdir']).strip())/'pg_lion.so'),
                    workload_sha256=hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest())
        (self.output/'queries.json').write_text(json.dumps(spec, indent=2) + '\n')
        def deadline(signum, frame):
            raise TimeoutError(f'Quick benchmark exceeded {self.args.max_seconds}s; results are incomplete')
        previous_handler = signal.signal(signal.SIGALRM, deadline)
        signal.alarm(self.args.max_seconds)
        try:
            self.cluster.start(initialize=True)
            self.db.query("SET statement_timeout='60s'")
            for extension in ['pg_lion', 'btree_gin', 'pg_visibility']:
                self.db.query(f'CREATE EXTENSION {extension}')
            meta['server_version'] = self.db.scalar('SELECT version()')
            meta['settings'] = {name: self.db.scalar('SHOW ' + name) for name in SETTINGS}
            (self.output/'metadata.json').write_text(json.dumps(meta, indent=2) + '\n')
            if self.args.baseline:
                compatible(meta, json.loads((Path(self.args.baseline)/'metadata.json').read_text()))
            for n in self.args.rows:
                self.run_dataset('scalar', n)
            if self.args.documents:
                self.run_dataset('documents', self.args.documents)
            meta['status'] = 'complete'
        except BaseException as error:
            meta['status'], meta['error'] = 'incomplete', repr(error)
            raise
        finally:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, previous_handler)
            self.samples.close()
            self.plans.close()
            self.events.close()
            try:
                self.close_owned()
            except BaseException as error:
                meta['status'], meta['error'] = 'incomplete', repr(error)
                raise
            finally:
                meta['elapsed_seconds'] = time.monotonic() - started
                meta['errors'] = self.errors
                (self.output/'metadata.json').write_text(json.dumps(meta, indent=2) + '\n')


def compatible(current, baseline):
    if baseline.get('status') != 'complete':
        raise ValueError('Baseline must be a completed quick run')
    fields = ['profile', 'workload_sha256', 'server_version', 'postgres_sha256', 'pg_config',
              'platform', 'cpu_model', 'cpu_count', 'settings']
    differences = [key for key in fields if current.get(key) != baseline.get(key)]
    differences += [key for key in ['repeats', 'warmups', 'seed']
                    if current['arguments'][key] != baseline['arguments'][key]]
    if differences:
        raise ValueError('Incompatible baseline: ' + ', '.join(differences))


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def summarize(directory, write_audit=True):
    meta = json.loads((directory/'metadata.json').read_text())
    if meta.get('profile') not in ['quick-v1', PROFILE] or meta.get('status') != 'complete':
        raise ValueError('Only completed quick runs can be reported or compared')
    samples, events = records(directory/'samples.jsonl'), records(directory/'operations.jsonl')
    key = lambda r: tuple(r[k] for k in KEY)
    expected = {key(r) for r in json.loads((directory/'queries.json').read_text())['configurations']}
    checks = [r for r in events if r['kind'] == 'correctness']
    if len(checks) != len(expected) or {key(r) for r in checks} != expected or any(r['status'] != 'pass' for r in checks):
        raise ValueError('Correctness-check coverage failed')
    digests = defaultdict(set)
    for r in checks:
        digests[(r['suite'], r['rows'], r['phase'], r['case'])].add(r['digest'])
    if any(len(v) != 1 for v in digests.values()):
        raise ValueError('Results differ across portfolios')
    with gzip.open(directory/'plans.jsonl.gz', 'rt') as stream:
        plan_ids = [json.loads(line)['sequence'] for line in stream]
    sample_ids = [r['sequence'] for r in samples]
    if len(set(plan_ids)) != len(plan_ids) or len(set(sample_ids)) != len(sample_ids) or set(plan_ids) != set(sample_ids):
        raise ValueError('Plan/sample correspondence failed')
    groups = defaultdict(list)
    for r in samples:
        if r['status'] != 'ok':
            raise ValueError('Failed timing in completed run')
        groups[key(r)].append(r)
    if set(groups) != expected:
        raise ValueError('Timing coverage failed')
    result = {}
    for k, values in groups.items():
        if len(values) != meta['arguments']['repeats'] or {r['repeat'] for r in values} != set(range(meta['arguments']['repeats'])):
            raise ValueError('Missing or duplicated timing rounds')
        times = [r['execution_ms'] for r in values]
        route = ' / '.join(sorted({'LionCount' if r['custom_count'] else ', '.join(r['scans']) for r in values}))
        result[k] = dict(zip(KEY, k), n=len(times), median_ms=statistics.median(times),
                         min_ms=min(times), max_ms=max(times),
                         planning_median_ms=statistics.median(r['planning_ms'] for r in values), plan=route)
    audit = dict(status='pass', samples=len(samples), correctness_checks=len(checks),
                 configurations=len(groups), cross_portfolio_result_groups=len(digests))
    if write_audit:
        (directory/'AUDIT.json').write_text(json.dumps(audit, indent=2) + '\n')
    return meta, result, events


def report(directory, baseline=None):
    meta, current, events = summarize(directory)
    old = {}
    if baseline:
        old_meta, old, _ = summarize(Path(baseline), write_audit=False)
        compatible(meta, old_meta)
    rows = []
    for k, row in sorted(current.items()):
        row = dict(row)
        prior = old.get(k)
        row.update(baseline_median_ms=prior['median_ms'] if prior else '',
                   change_pct=(row['median_ms']/prior['median_ms']-1)*100 if prior and prior['median_ms'] else '',
                   baseline_plan=prior['plan'] if prior else '',
                   plan_changed=prior['plan'] != row['plan'] if prior else '')
        rows.append(row)
    with (directory/'summary.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)
    headers = ['Suite', 'Rows', 'Variant', 'State', 'Planner', 'Memory', 'Query', 'n',
               'Median ms', 'Min ms', 'Max ms', 'Planning ms', 'Plan', 'Baseline ms', 'Change %', 'Baseline plan', 'Plan changed']
    def cell(value):
        return f'{value:.3f}' if isinstance(value, float) else str(value)
    body = [[cell(v) for v in r.values()] for r in rows]
    def table(head, data):
        return '| ' + ' | '.join(head) + ' |\n| ' + ' | '.join(['---']*len(head)) + ' |\n' + ''.join(
            '| ' + ' | '.join(cell(v).replace('|', '\\|') for v in row) + ' |\n' for row in data)
    builds = defaultdict(float)
    for r in events:
        if r['kind'] == 'build':
            builds[(r['suite'], r['rows'], r['family'])] += r['elapsed_ms']
    sizes = [[r['suite'], r['rows'], r['family'], builds[(r['suite'], r['rows'], r['family'])], r['index_bytes']/2**20]
             for r in events if r['kind'] == 'portfolio']
    maintenance = [[r['rows'], r['family'], r['kind'], r['elapsed_ms'], r['wal_bytes']/2**20]
                   for r in events if r['kind'] in ['insert', 'indexed_update', 'vacuum', 'delete', 'vacuum_after_delete', 'mid_insert', 'churn_update', 'churn_vacuum']]
    intro = (f"# Quick index progress benchmark\n\nCommit `{meta['commit']}`. Profile `{meta['profile']}`. "
             f"Completed in **{meta['elapsed_seconds']:.1f} seconds**, with **{len(rows)} exact-result checks** "
             f"and **{sum(r['n'] for r in rows)} timings**. All checks pass.\n\n"
             "Only B-tree, GIN, roaring, and roaring_bitmap are timed. Documents use GIN and roaring; "
             "B-tree has no matching array/full-text index here. Sequential scans supply untimed correctness references. "
             f"{meta['arguments']['repeats']} timing rounds, {meta['arguments']['warmups']} warmup(s), one index build; "
             "warm cache, single-client queries, parallel query/JIT off. "
             "Small-sample medians/min/max are progress signals, not confidence intervals or production capacity estimates. "
             "Build and maintenance times are single observations.\n\n"
             "Read timings are EXPLAIN execution time; planning is separate. Positive change means slower than the baseline. "
             "Inspect plan changes and repeat suspicious differences, especially sub-millisecond results. "
             "Different commits/extension binaries are expected; incompatible workloads/server settings are rejected.\n\n")
    if baseline:
        intro += f"Baseline: `{Path(baseline).resolve()}`, commit `{old_meta['commit']}`.\n\n"
    sections = [('Build and size', ['Suite', 'Rows', 'Family', 'Build ms', 'Indexes MiB'], sizes),
                ('Maintenance', ['Rows', 'Family', 'Operation', 'Elapsed ms', 'WAL MiB'], maintenance),
                ('Query timings and baseline comparison', headers, body)]
    markdown = intro + ''.join('## '+title+'\n\n'+table(head, data)+'\n' for title, head, data in sections)
    (directory/'REPORT.md').write_text(markdown)
    doc = '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Quick index benchmark</title><style>body{font:14px/1.5 system-ui;margin:24px;color:#173047}table{border-collapse:collapse}th,td{padding:7px;border-bottom:1px solid #ddd;white-space:nowrap;text-align:left}th{background:#173047;color:white;position:sticky;top:0}.scroll{overflow:auto;max-height:650px}pre{white-space:pre-wrap}</style><h1>Quick index progress benchmark</h1><p><a href="REPORT.md">Report and methodology</a> · <a href="summary.csv">CSV and baseline deltas</a> · <a href="AUDIT.json">Audit</a> · <a href="metadata.json">Environment</a></p>'
    doc += '<pre>' + html.escape(intro) + '</pre>'
    for title, head, data in sections:
        doc += '<h2>'+html.escape(title)+'</h2><div class="scroll"><table><thead><tr>'+''.join('<th>'+html.escape(v)+'</th>' for v in head)+'</tr></thead><tbody>'
        doc += ''.join('<tr>'+''.join('<td>'+html.escape(cell(v))+'</td>' for v in row)+'</tr>' for row in data)
        doc += '</tbody></table></div>'
    (directory/'index.html').write_text(doc+'</html>')
    (directory/'SHA256SUMS').write_text(''.join(sha(p)+'  '+p.name+'\n' for p in sorted(directory.iterdir())
                                               if p.is_file() and p.name != 'SHA256SUMS'))
    print(f"{directory}/index.html: {meta['elapsed_seconds']:.1f}s, {len(rows)} checks, {sum(r['n'] for r in rows)} timings")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', help='Installed PostgreSQL with current pg_lion, btree_gin, pg_visibility')
    parser.add_argument('--output', help='New directory; defaults to bench/results/quick/<UTC timestamp>-<commit>')
    parser.add_argument('--baseline', help='Completed quick run with matching workload/environment')
    parser.add_argument('--report-only', metavar='DIRECTORY', help='Audit/regenerate saved results or compare them without starting PostgreSQL')
    parser.add_argument('--rows', type=int, nargs='+', default=[1000000, 5000000], help='Scalar sizes (default: 1000000 5000000)')
    parser.add_argument('--documents', type=int, default=200000, help='Document rows (default: 200000); 0 omits the document matrix')
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--warmups', type=int, default=1)
    parser.add_argument('--seed', type=int, default=20260921)
    parser.add_argument('--max-seconds', type=int, default=300, help='Measurement deadline; aborts incomplete runs (plus query cancellation/cleanup)')
    args = parser.parse_args()
    if args.report_only:
        report(Path(args.report_only).resolve(), args.baseline)
        return
    if not args.prefix:
        parser.error('--prefix is required unless --report-only is used')
    if min(*args.rows, args.repeats, args.max_seconds) < 1 or min(args.documents, args.warmups) < 0:
        parser.error('rows, repeats, and max-seconds must be positive; documents/warmups must be nonnegative')
    if len(set(args.rows)) != len(args.rows):
        parser.error('--rows must not contain duplicate sizes')
    if not args.output:
        commit = command(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT).strip()
        args.output = str(ROOT/'bench/results/quick'/f"{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}-{commit}")
    args.resume, args.keep_cluster = False, False
    args.build_repeats = 1
    suite = QuickSuite(args)
    suite.run()
    report(suite.output, args.baseline)


if __name__ == '__main__':
    main()
