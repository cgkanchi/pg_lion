#!/usr/bin/env python3
"""Check artifact integrity and cross-portfolio result agreement after a run."""
import argparse
from collections import defaultdict
import gzip
import hashlib
import json
from pathlib import Path


def expected_configurations(meta, queries):
    """Derive coverage from the saved workload, not only from existing samples."""
    warm, cold = set(), set()
    args = meta['arguments']
    datasets = [('scalar', n) for n in args['rows']]
    if args['documents']:
        datasets.append(('documents', args['documents']))
    for suite, n in datasets:
        for family in args['families']:
            if suite == 'documents' and family not in ['seq', 'gin', 'gist', 'roaring']:
                continue
            variants = ['roaring', 'roaring_bitmap'] if family == 'roaring' else [family]
            for variant in variants:
                if 'matrix' in queries:
                    for config in queries['matrix'][suite]:
                        for case in config['cases']:
                            warm.add((suite, n, variant, config['phase'], config['mode'], config['memory'], case))
                    if suite == 'scalar' and args['cold_repeats']:
                        for case in ['eq_c200_17', 'fetch_medium']:
                            cold.add((suite, n, variant, 'shared_buffers_cold', 'default', '64MB', case))
                    continue
                for case in queries[suite]:
                    for mode in ['default', 'prefer_index']:
                        warm.add((suite, n, variant, 'clean', mode, '64MB', case['id']))
                        if suite == 'scalar' and case['stress']:
                            for phase in ['dirty_clustered_5pct', 'dirty_scattered']:
                                warm.add((suite, n, variant, phase, mode, '64MB', case['id']))
                if suite == 'scalar':
                    for case in ['eq_c2_0', 'in_c20k_1000', 'group_c200', 'fetch_medium']:
                        warm.add((suite, n, variant, 'low_work_mem', 'prefer_index', '64kB', case))
                    if args['maintenance']:
                        for case in ['eq_c200_17', 'and2', 'is_null', 'group_c200']:
                            warm.add((suite, n, variant, 'after_maintenance', 'default', '64MB', case))
                    if args['cold_repeats']:
                        for case in ['eq_c200_17', 'fetch_medium']:
                            cold.add((suite, n, variant, 'shared_buffers_cold', 'default', '64MB', case))
    return warm, cold


def manifest(directory):
    """Hash reports, raw artifacts, and the exact harness sources."""
    paths = [p for p in directory.iterdir() if p.is_file() and p.name != 'SHA256SUMS']
    sources = list(Path(__file__).parent.glob('*.py'))
    import os
    lines = []
    for path in sorted(paths + sources):
        sha = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                sha.update(block)
        lines.append(sha.hexdigest() + '  ' + os.path.relpath(path, directory))
    (directory / 'SHA256SUMS').write_text('\n'.join(lines) + '\n')


def audit_stress(directory, meta):
    args = meta['arguments']
    records = [json.loads(x) for x in (directory / 'stress.jsonl').read_text().splitlines()]
    methods = args.get('families', ['seq', 'btree', 'hash', 'gin', 'gist', 'brin', 'roaring'])
    expected = set()
    for method in methods:
        variants = ['roaring', 'roaring_bitmap'] if method == 'roaring' else [method]
        for variant in variants:
            for order in ['bulk_then_index', 'index_then_insert']:
                for query in ['point', 'dense', 'in_100']:
                    expected.add(('growth', variant, order, query))
            for cycle in range(1, args['cycles'] + 1):
                for query in ['point', 'obsolete', 'dense']:
                    expected.add(('churn', variant, cycle, query))
        dirty_variants = variants + (['roaring_sql'] if method == 'roaring' else [])
        for variant in dirty_variants:
            for memory in ['64kB', '4MB', '64MB']:
                for query in ['dense', 'group']:
                    expected.add(('dirty_memory', variant, memory, query))
        for mode in ['force_custom_plan', 'force_generic_plan']:
            for query in ['point', 'dense']:
                expected.add(('prepared', method, mode, query))
    def key(r):
        state = r.get('order', r.get('memory', r.get('plan_mode', r.get('cycle'))))
        return r['profile'], r['variant'], state, r['query']
    checks = [r for r in records if r['kind'] == 'correctness']
    if len(checks) != len(expected) or {key(r) for r in checks} != expected or any(r['status'] != 'pass' for r in checks):
        raise ValueError('Supplement correctness coverage failed')
    groups = defaultdict(list)
    for r in records:
        if r['kind'] == 'query':
            groups[key(r)].append(r)
    if set(groups) != expected:
        raise ValueError('Supplement query coverage failed')
    for k, values in groups.items():
        if len(values) != args['repeats'] or {r['repeat'] for r in values} != set(range(args['repeats'])):
            raise ValueError(f'Supplement timing rounds incomplete: {k}')
        if any('Plan' not in r['plan'] or r['execution_ms'] < 0 for r in values):
            raise ValueError(f'Supplement plan/timing invalid: {k}')
    writer_keys = {(m, c) for m in methods for c in args['clients']} if args['duration'] else set()
    writers = [r for r in records if r['kind'] == 'writers']
    if len(writers) != len(writer_keys) or {(r['method'], r['clients']) for r in writers} != writer_keys:
        raise ValueError('Supplement writer coverage failed')
    for r in writers:
        if r['rows'] != 100 * r['transactions'] or r['correctness'] != 'row_count_pass':
            raise ValueError('Supplement writer row count failed')
    indexed = [r for r in records if r['kind'] == 'writer_index_check']
    indexed_keys = {(m, c, k) for m, c in writer_keys for k in [0, 1]}
    if len(indexed) != len(indexed_keys) or {(r['method'], r['clients'], r['key']) for r in indexed} != indexed_keys:
        raise ValueError('Supplement post-write index coverage failed')
    if any(r['status'] != 'pass' or r['actual'] != r['expected'] for r in indexed):
        raise ValueError('Supplement post-write index count failed')
    output = dict(status='pass', samples=sum(len(v) for v in groups.values()),
                  query_configurations=len(groups), correctness_checks=len(checks),
                  writer_bursts=len(writers), writer_index_checks=len(indexed),
                  note='Complete declared query matrix and indexed post-write checks; not an endurance test.')
    (directory / 'AUDIT.json').write_text(json.dumps(output, indent=2) + '\n')
    manifest(directory)
    print(json.dumps(output, indent=2))


def audit(directory):
    directory=Path(directory)
    meta=json.loads((directory/'metadata.json').read_text())
    if meta.get('status') not in ['complete','complete_with_errors']:
        raise ValueError('Run is not complete')
    if (directory/'stress.jsonl').exists():
        return audit_stress(directory, meta)
    samples=[json.loads(x) for x in (directory/'samples.jsonl').read_text().splitlines()]
    operations=[json.loads(x) for x in (directory/'operations.jsonl').read_text().splitlines()]
    sample_ids=[s['sequence'] for s in samples]
    if len(sample_ids)!=len(set(sample_ids)):
        raise ValueError('Duplicate sample sequence IDs')
    with gzip.open(directory/'plans.jsonl.gz','rt') as f:
        plan_ids=[json.loads(line)['sequence'] for line in f]
    if len(plan_ids)!=len(set(plan_ids)) or set(plan_ids)!={s['sequence'] for s in samples if s['status']=='ok'}:
        raise ValueError('Plan/sample correspondence failed')
    groups=defaultdict(list)
    for s in samples:
        key=tuple(s[k] for k in ['suite','rows','variant','phase','mode','memory','case'])
        groups[key].append(s)
    warm, cold = expected_configurations(meta, json.loads((directory/'queries.json').read_text()))
    check_records = [op for op in operations if op['kind'] == 'correctness']
    skipped_records = [op for op in operations if op['kind'] == 'skipped_configuration']
    def check_key(op):
        return tuple(op[k] for k in ['suite','rows','variant','phase','mode','memory','case'])
    skipped = {check_key(op) for op in skipped_records}
    checked = {check_key(op) for op in check_records}
    if (len(check_records) != len(checked) or len(skipped_records) != len(skipped) or
        checked & skipped or checked | skipped != warm):
        raise ValueError('Declared correctness-check matrix is incomplete or duplicated')
    for op in skipped_records:
        family = 'roaring' if op['variant'] == 'roaring_bitmap' else op['variant']
        if op['phase'] != 'after_maintenance' or not any(
            e.get('suite') == op['suite'] and e.get('rows') == op['rows'] and e.get('family') == family and
            e['kind'] in ['insert','indexed_update','delete','vacuum','reindex'] and e.get('status') == 'error'
            for e in operations):
            raise ValueError('Skipped configuration lacks a recorded maintenance failure')
    passed = {check_key(op) for op in check_records if op['status'] == 'pass'}
    if set(groups) != passed | cold:
        raise ValueError('Declared timing matrix is incomplete or has unexpected configurations')
    for key,values in groups.items():
        expected=meta['arguments']['cold_repeats'] if key[3]=='shared_buffers_cold' else meta['arguments']['repeats']
        if len(values)!=expected or {s['repeat'] for s in values}!=set(range(expected)):
            raise ValueError(f'Incomplete or duplicated rounds: {key}')
    digests=defaultdict(set)
    for op in operations:
        if op['kind']=='correctness' and op['status']=='pass':
            key=tuple(op[k] for k in ['suite','rows','phase','case'])
            digests[key].add(op['digest'])
    disagreement=[key for key,values in digests.items() if len(values)!=1]
    if disagreement:
        raise ValueError(f'Regenerated datasets / query results differ across portfolios: {disagreement}')
    failures=[op for op in operations if op.get('status')=='error']+[s for s in samples if s['status']=='error']
    output=dict(status='pass',samples=len(samples),plans=len(plan_ids),query_configurations=len(groups),
                correctness_checks=len(check_records),
                explicitly_skipped_configurations=len(skipped),
                cross_portfolio_result_groups=len(digests),recorded_failures=len(failures),
                failure_types=sorted({op.get('kind','query') for op in failures}),
                note='Artifact consistency; recorded build/query errors remain errors. Not proof of untested semantics.')
    (directory/'AUDIT.json').write_text(json.dumps(output,indent=2)+'\n')
    manifest(directory)
    print(json.dumps(output,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory')
    audit(parser.parse_args().directory)
