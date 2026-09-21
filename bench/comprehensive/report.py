#!/usr/bin/env python3
"""Produce an exhaustive Markdown table, standalone searchable HTML, and SVG plots."""
import argparse
from collections import defaultdict
import csv
import html
import io
import json
import os
from pathlib import Path
import random
import re
import statistics


def percentile(values, p):
    v = sorted(values)
    pos = (len(v)-1)*p
    lo = int(pos)
    return v[lo]+(v[min(lo+1,len(v)-1)]-v[lo])*(pos-lo)


def summary(values):
    rng = random.Random(20260920)
    boots = sorted(statistics.median(rng.choices(values,k=len(values))) for _ in range(2000))
    return dict(n=len(values), median=statistics.median(values), p95=percentile(values,.95),
                lo=percentile(boots,.025), hi=percentile(boots,.975),
                cv=statistics.stdev(values)/statistics.mean(values) if len(values)>1 and statistics.mean(values) else 0)


def read_jsonl(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def table(headers, rows):
    return '| '+' | '.join(headers)+' |\n| '+' | '.join(['---']*len(headers))+' |\n'+''.join(
        '| '+' | '.join(str(c) for c in row)+' |\n' for row in rows)


def svg_bars(title, values, unit):
    os.environ.setdefault('MPLCONFIGDIR','/tmp/lion-benchmark-matplotlib')
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    matplotlib.rcParams.update({'svg.fonttype':'none','svg.hashsalt':title,'font.size':10})
    figure,axis=plt.subplots(figsize=(9,max(2,len(values)*.35+1.2)))
    names=[name for name,_ in values]
    numbers=[value for _,value in values]
    axis.barh(names,numbers,color=['#d56435' if name.startswith('roaring') else '#376f98' for name in names])
    axis.invert_yaxis()
    maximum=max(numbers,default=1) or 1
    for i,value in enumerate(numbers):
        axis.text(value+maximum*.015,i,f'{value:.3g}',va='center',fontsize=9)
    axis.set_xlim(0,maximum*1.18)
    axis.set_xlabel(unit)
    axis.set_title(title,loc='left',fontsize=11,pad=12)
    axis.spines[['top','right','left']].set_visible(False)
    axis.grid(axis='x',alpha=.18)
    axis.set_axisbelow(True)
    figure.tight_layout()
    result=io.StringIO()
    figure.savefig(result,format='svg',metadata={'Date':None,'Title':title})
    plt.close(figure)
    svg=result.getvalue()
    return svg[svg.index('<svg'):]


def generate(directory):
    directory=Path(directory)
    meta=json.loads((directory/'metadata.json').read_text())
    samples=read_jsonl(directory/'samples.jsonl')
    events=read_jsonl(directory/'operations.jsonl')
    grouped=defaultdict(list)
    fields=['suite','rows','phase','mode','memory','case','variant']
    for s in samples:
        if s['status']=='ok':
            grouped[tuple(s[f] for f in fields)].append(s)
    stats={key:summary([s['execution_ms'] for s in ss]) for key,ss in grouped.items()}
    rows=[]
    for key in sorted(stats):
        st=stats[key]
        ss=grouped[key]
        base=stats.get(key[:-1]+('seq',))
        ratio=base['median']/st['median'] if base and st['median'] else None
        if any(s['custom_count'] for s in ss):
            access='LionCount'
        else:
            access=', '.join(sorted({v for s in ss for v in s['scans']})) or 'no scan'
        if any(s['fallback'] for s in ss):
            access+=' (fallback)'
        rows.append(list(key)+[st['n'],f"{st['median']:.3f}",f"{st['lo']:.3f}–{st['hi']:.3f}",
                              f"{st['p95']:.3f}",f"{st['cv']:.1%}",f'{ratio:.2f}' if ratio else '—',access,
                              f"{statistics.median(s['planning_ms'] for s in ss):.3f}",
                              f"{statistics.median(s['planning_ms']+s['execution_ms'] for s in ss):.3f}"])
    headers=['Suite','Rows','State','Planner','work_mem','Query','Variant','n','Execution median ms','95% CI ms','p95 ms','CV','Speedup vs seq','Access','Planning median ms','Plan+execution median ms']
    errors=[e for e in events if e.get('status')=='error']+[s for s in samples if s['status']=='error']
    checks=sum(e.get('kind')=='correctness' and e.get('status')=='pass' for e in events)
    skipped=[e for e in events if e['kind']=='skipped_configuration']
    partial={(e['suite'],e['rows'],e['family']):e.get('failed_indexes',[]) for e in events
             if e['kind']=='portfolio' and e.get('completeness')=='partial'}
    def family_name(suite,n,variant):
        family='roaring' if variant=='roaring_bitmap' else variant
        return variant+(' (partial)' if (suite,n,family) in partial else '')
    for row in rows:
        row[6]=family_name(row[0],row[1],row[6])
    cpu=next((line.split(':',1)[1].strip() for line in meta['cpuinfo'].splitlines() if line.startswith('model name')),'unknown')
    ram=int(next(line.split()[1] for line in meta['meminfo'].splitlines() if line.startswith('MemTotal:')))/2**20
    intro=f"""# Index comparison: measured results

Commit `{meta['commit']}`. Run status: **{meta.get('status','incomplete')}**. Started {meta['started_utc']}; completed {meta.get('completed_utc','not completed')}.

{len(samples):,} timed observations, {checks:,} successful exact result-multiset checks, **{len(errors)} recorded errors**, and **{len(skipped)} explicitly unmeasured configurations** after maintenance failures. A correctness check is separate from EXPLAIN timing; the suite does not mistake an aggregate's output row count for a proof of correctness. An operation timeout is a failed attempt, not a successful duration; dependent checks are skipped and identified below.

**Build failures:** {len([e for e in errors if e.get('kind')=='build'])} recorded failed index builds. Statements have a 60-second limit. A failed index is omitted from the remaining portfolio and is not retried in subsequent build rounds; the family remains explicitly partial. Query results for that family use its surviving indexes or a planner fallback. They are **not measurements of the missing index**. See the completeness column and individual-index status below. Resume history and any earlier fatal attempt are preserved in metadata.

Hardware: **{cpu}**, {meta['cpu_count']} logical CPUs, {ram:.2f} GiB visible RAM. Server: `{meta.get('server_version','unknown')}`. Platform: `{meta['platform']}`. See [metadata.json](metadata.json) for compiler flags, all server settings, seed, and run arguments. Shared buffers are 512 MB; normal work_mem is 64 MB and maintenance_work_mem is 512 MB. Parallel query and JIT are disabled to isolate access paths; index builds use the recorded maintenance-worker setting. Durability is enabled. Autovacuum is disabled; vacuum and visibility transitions are explicit.

This is a synthetic, single-machine comparison, inspired by the presentation of the [Biscuit benchmark](https://biscuit.readthedocs.io/en/latest/benchmark.html). It measures this extension's integer, array, and full-text workloads, not Biscuit's wildcard workload. The data and workload SQL are in [queries.json](queries.json); generation and execution are in the [runner](../../comprehensive/run.py).

## Interpretation and fairness

Every scalar family attempts the same eight single-column indexes on a freshly generated heap; exceptions are explicitly partial portfolios as described above. `btree_tuned` additionally receives a composite `(c200,c20,c2)` index and a covering `(c200) INCLUDE (id,payload)` index; its larger portfolio is charged in build time, bytes, and maintenance. Scalar GIN and GiST use the official `btree_gin` and `btree_gist` operator classes. GIN fastupdate is on; BRIN uses 32-page ranges. Roaring uses extension defaults. `roaring_bitmap` reuses the roaring portfolio with count pushdown disabled.

Document comparisons use native GIN, GiST, and roaring text-search support. GIN and roaring also index text arrays. **GiST has no text[] containment opclass here**, so its array queries are explicitly a fallback with no array index. Hash cannot accelerate ordering/ranges, and roaring scalar indexes do not provide ordered/range access. Their measured query plans are retained rather than pretending that these operations are supported. SP-GiST's built-in spatial/radix operator classes do not provide an equivalent integer, text[] containment, or tsvector portfolio, so SP-GiST is outside this workload. This suite does not claim coverage of geometric, vector, JSON, wildcard, or arbitrary extension indexes. See PostgreSQL's [index types](https://www.postgresql.org/docs/current/indexes-types.html), [btree_gin](https://www.postgresql.org/docs/current/btree-gin.html), [btree_gist](https://www.postgresql.org/docs/current/btree-gist.html), and [text-search indexes](https://www.postgresql.org/docs/current/textsearch-indexes.html).

`default` uses normal planner preferences. `prefer_index` disables sequential scans as a diagnostic; it is not a production tuning recommendation and does not guarantee use of an index. The access column and raw plans identify the actual route. A fallback is a query execution result, not a successful measurement of the named index algorithm.

Each query is verified against a forced sequential-scan result, warmed independently, and measured in randomized complete rounds within a portfolio. Families are shuffled with a fixed seed. Families run sequentially; there is no interleaved cross-family crossover trial. Warm means no deliberate cache eviction, **not a guarantee that the working set fits shared buffers**. `shared_buffers_cold` restarts the dedicated server before each measured query; the OS page cache remains warm, and planning can warm metadata pages before execution. No claim about cold physical-disk performance is made.

`clean` follows VACUUM FREEZE. `dirty_clustered_5pct` updates the first 5% of rows' non-indexed payload; `dirty_scattered` additionally updates every hundredth row. Names describe mutations, not an assumed visibility percentage; [operations.jsonl](operations.jsonl) records measured all-visible and heap-page counts. `low_work_mem` uses 64 kB and records lossy bitmap/temp-block evidence in [samples.jsonl](samples.jsonl). The normal setting is 64 MB. Maintenance is measured once per portfolio after these visibility mutations, with a checkpoint before each operation; WAL includes full-page images and index WAL. Build repeats are recorded separately. Insert batches contain 1% of initial rows; indexed updates affect the first 1%, and deletes use `id % 100 = 1`.

Read latency is server EXPLAIN ANALYZE execution time with per-node timing off, including EXPLAIN instrumentation but excluding planning and result transfer. Planning time and full buffer/WAL plans are retained in [samples.jsonl](samples.jsonl) and [plans.jsonl.gz](plans.jsonl.gz). Medians, interpolated p95, sample CV, and a seeded 2,000-resample percentile bootstrap 95% CI of the median are shown. With few samples, especially cold samples, intervals and p95 are descriptive and imprecise. Speedups divide matching sequential-scan medians by the variant median; they are not cross-workload averages. Sub-millisecond ratios are especially sensitive to timer resolution and instrumentation overhead. Timeout/error observations remain errors and are excluded from latency summaries, never converted to zero.

Concurrency measures closed-loop SELECTs through Python threads and synchronous libpq, including client dispatch, result transfer, and decoding. It can become client-bound for very fast counts; it is not a maximum server-throughput claim. The separate [growth/churn/write supplement](../2026-09-21-stress/REPORT.md) covers empty-index growth, changing-key churn, prepared statements, fully dirty low-memory counts, and short concurrent-write bursts. Crash recovery, replication, filesystem cache eviction, repeated maintenance trials, variable GIN pending-list settings, and tuning sweeps remain outside these runs. The benchmark is exhaustive over its published matrix, not every PostgreSQL workload.

Storage columns labeled Heap MiB use `pg_table_size`, including auxiliary forks and any TOAST storage; portfolio bytes use `pg_indexes_size`. Individual-index bytes use `pg_relation_size` (main fork). MiB means 1,048,576 bytes. Consult the [results guide](../../COMPARISON.md) for selected comparisons and their practical limits.
"""
    portfolio=[e for e in events if e['kind']=='portfolio']
    builds=defaultdict(float)
    for e in events:
        if e['kind']=='build':
            builds[(e['suite'],e['rows'],e['family'],e['repeat'])]+=e['elapsed_ms']
    buildstats=defaultdict(list)
    for (suite,n,family,rep),ms in builds.items():
        buildstats[(suite,n,family)].append(ms)
    size_rows=[]
    for e in sorted(portfolio,key=lambda e:(e['suite'],e['rows'],e['family'])):
        b=buildstats.get((e['suite'],e['rows'],e['family']),[0])
        size_rows.append([e['suite'],e['rows'],e['family'],f"{e['heap_bytes']/2**20:.2f}",
                          f"{e['index_bytes']/2**20:.2f}",
                          '—' if (e['suite'],e['rows'],e['family']) in partial else f'{statistics.median(b)/1000:.3f}',
                          len(b) if b!=[0] else 0,
                          'partial: '+', '.join(e['failed_indexes']) if e.get('failed_indexes') else 'complete'])
    size_headers=['Suite','Rows','Family','Heap MiB','Indexes MiB','Median build seconds','Build rounds','Completeness / omitted indexes']
    perindex=defaultdict(list)
    perindexsize={}
    for e in events:
        if e['kind']=='build':
            perindex[(e['suite'],e['rows'],e['family'],e['index'])].append(e)
        elif e['kind']=='index_size':
            perindexsize[(e['suite'],e['rows'],e['family'],e['index'])]=e['bytes']
    index_headers=['Suite','Rows','Family','Index','MiB','Median attempt ms','Min–max attempt ms','Median WAL MiB','Attempts','Status']
    index_rows=[]
    for key,records in sorted(perindex.items()):
        times=[e['elapsed_ms'] for e in records]
        index_rows.append(list(key)+[f'{perindexsize[key]/2**20:.3f}' if key in perindexsize else '—',f'{statistics.median(times):.3f}',
                          f'{min(times):.3f}–{max(times):.3f}',
                          f"{statistics.median(e['wal_bytes'] for e in records)/2**20:.3f}",len(records),
                          'FAILED / omitted' if any(e.get('status')=='error' for e in records) else 'built'])
    vm_headers=['Suite','Rows','Family','State','Heap pages','All-visible pages','All-visible %']
    vm_rows=[[e['suite'],e['rows'],e['family'],e['phase'],e['heap_pages'],e['all_visible'],
              f"{100*int(e['all_visible'])/max(1,e['heap_pages']):.2f}"] for e in events if e['kind']=='visibility']
    maint=[e for e in events if e['kind'] in ['insert','indexed_update','delete','vacuum','reindex']]
    maint_sizes={(e['rows'],e['family'],e['operation']):e['index_bytes'] for e in events if e['kind']=='maintenance_size'}
    maint_headers=['Rows','Family','Operation','Elapsed attempt ms','WAL MiB','Indexes MiB after','Status']
    maint_rows=[[e['rows'],family_name('scalar',e['rows'],e['family']),e['kind'],f"{e['elapsed_ms']:.3f}",f"{e['wal_bytes']/2**20:.3f}",
                 f"{maint_sizes[(e['rows'],e['family'],e['kind'])]/2**20:.3f}" if (e['rows'],e['family'],e['kind']) in maint_sizes else '—',
                 e.get('status','ok')] for e in maint]
    conc=[e for e in events if e['kind']=='concurrency']
    conc_headers=['Rows','Variant','Clients','Query','Transactions/s','Median ms','p95 ms','Duration s']
    conc_rows=[[e['rows'],family_name('scalar',e['rows'],e['variant']),e['clients'],e['case'],f"{e['tps']:.1f}",f"{e['median_ms']:.3f}",f"{e['p95_ms']:.3f}",f"{e['duration_seconds']:.2f}"] for e in conc]
    charts=[]
    for n in sorted({e['rows'] for e in portfolio if e['suite']=='scalar'}):
        values=[(e['family']+(' (partial)' if e.get('failed_indexes') else ''),e['index_bytes']/2**20) for e in portfolio if e['suite']=='scalar' and e['rows']==n]
        title=f'Scalar portfolio size, {n:,} rows (lower is smaller)'
        svg=svg_bars(title,sorted(values,key=lambda v:v[1]),'MiB')
        name=f'size-{n}.svg'
        (directory/name).write_text(svg)
        charts.append((title,name,svg))
        for case in ['eq_c2_0','eq_c200_17','and3','group_c200','range_random','fetch_medium']:
            values=[(key[-1]+(' (partial)' if (key[0],key[1],key[-1]) in partial else ''),st['median']) for key,st in stats.items()
                    if key[:6]==('scalar',n,'clean','default','64MB',case)]
            if not values:
                continue
            title=f'{case}, {n:,} clean rows, default planner (lower is faster)'
            svg=svg_bars(title,sorted(values,key=lambda v:v[1]),'ms')
            name=f'{case}-{n}.svg'
            (directory/name).write_text(svg)
            charts.append((title,name,svg))
    markdown=intro+'\n## Size and build cost\n\n'+table(size_headers,size_rows)
    markdown+='\n## Every individual index\n\n'+table(index_headers,index_rows)
    markdown+='\n## Measured visibility\n\n'+table(vm_headers,vm_rows)
    markdown+='\n## Selected plots\n\n'+''.join(f'![{title}]({name})\n\n' for title,name,_ in charts)
    markdown+='\n## Maintenance (single observations)\n\n'+table(maint_headers,maint_rows)
    markdown+='\n## Concurrent reads\n\n'+table(conc_headers,conc_rows)
    markdown+='\n## Every query and configuration\n\n'+table(headers,rows)
    markdown+='\n## Errors\n\n'+('None.\n' if not errors else '```json\n'+json.dumps(errors,indent=2)+'\n```\n')
    markdown+='\n## Explicitly unmeasured configurations\n\n'+('None.\n' if not skipped else '```json\n'+json.dumps(skipped,indent=2)+'\n```\n')
    (directory/'REPORT.md').write_text(markdown)
    for filename,head,body in [('summary.csv',headers,rows),('indexes.csv',index_headers,index_rows),
                               ('maintenance.csv',maint_headers,maint_rows),('concurrency.csv',conc_headers,conc_rows)]:
        with (directory/filename).open('w',newline='') as f:
            writer=csv.writer(f)
            writer.writerow(head)
            writer.writerows(body)
    def htmltable(head,body,ident):
        return f'<div class="scroll"><table id="{ident}"><thead><tr>'+''.join(f'<th>{html.escape(str(x))}</th>' for x in head)+'</tr></thead><tbody>'+''.join('<tr>'+''.join(f'<td>{html.escape(str(x))}</td>' for x in row)+'</tr>' for row in body)+'</tbody></table></div>'
    # The small, generated methodology needs only paragraphs, headings, and inline markup.
    def inline(text):
        text=html.escape(text)
        text=re.sub(r'`([^`]+)`',r'<code>\1</code>',text)
        text=re.sub(r'\*\*([^*]+)\*\*',r'<strong>\1</strong>',text)
        return re.sub(r'\[([^\]]+)\]\(([^\s)]+)\)',r'<a href="\2">\1</a>',text)
    paragraphs=''.join('<h3>'+inline(p[3:])+'</h3>' if p.startswith('## ') else
                       '<p>'+inline(p)+'</p>' for p in intro.split('\n\n') if not p.startswith('# '))
    document='''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>PostgreSQL index comparison</title><style>
body{font:15px/1.55 system-ui,sans-serif;color:#173047;background:#f4f7fa;margin:0}main{max-width:1500px;margin:auto;padding:30px}h1,h2{line-height:1.2}p{max-width:1050px}table{border-collapse:collapse;background:white;font-size:13px;width:100%}td,th{padding:7px 10px;border-bottom:1px solid #dbe3ea;text-align:left;white-space:nowrap}th{position:sticky;top:0;background:#173047;color:white}tbody tr:hover{background:#edf6fe}.scroll{overflow:auto;max-height:700px;margin:20px 0}.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(480px,1fr));gap:20px}svg{width:100%;border:1px solid #dbe3ea}input,select{padding:10px;font:inherit}input{width:min(650px,90%)}.filters{display:flex;gap:12px;flex-wrap:wrap}.filters label{display:flex;flex-direction:column;font-size:12px}summary{cursor:pointer;font-weight:bold}a{color:#245e8e}th button{background:none;border:none;color:inherit;font:inherit;cursor:pointer}code{background:#e6edf3;padding:1px 4px}@media(max-width:550px){main{padding:14px}.charts{display:block}}
</style><main><h1>PostgreSQL index comparison</h1>'''
    document+=f'<p>{len(samples):,} observations · {checks:,} correctness checks · {len(errors)} errors · {len(skipped)} explicitly unmeasured configurations · commit {meta["commit"][:12]}</p>'
    document+='<p><a href="REPORT.md">Complete Markdown report</a> · <a href="summary.csv">Summary CSV</a> · <a href="metadata.json">Environment</a> · <a href="queries.json">SQL</a> · <a href="samples.jsonl">Raw samples</a> · <a href="operations.jsonl">Build/maintenance records</a> · <a href="plans.jsonl.gz">All query plans</a></p>'
    document+='<details><summary>Methodology, applicability, and limitations</summary>'+paragraphs+'</details>'
    document+='<h2>Size and build cost</h2>'+htmltable(size_headers,size_rows,'sizes')
    document+='<details><summary>Every individual index: size, build time, and WAL</summary>'+htmltable(index_headers,index_rows,'indexes')+'</details>'
    document+='<details><summary>Measured visibility map coverage</summary>'+htmltable(vm_headers,vm_rows,'visibility')+'</details>'
    document+='<h2>Selected comparisons</h2><div class="charts">'+''.join(svg for _,_,svg in charts)+'</div>'
    document+='<h2>Maintenance</h2>'+htmltable(maint_headers,maint_rows,'maintenance')
    document+='<h2>Concurrent reads</h2>'+htmltable(conc_headers,conc_rows,'concurrent')
    document+='<h2>Every query and configuration</h2><div class="filters" id="selectors"></div><p><label for="filter">Search query or any row text</label><br><input id="filter" placeholder="e.g. and3"><span id="count" role="status"></span></p><p>Click a column heading to sort. Speedups use the matching sequential baseline.</p>'+htmltable(headers,rows,'results')
    document+='<h2>Errors</h2><pre>'+html.escape(json.dumps(errors,indent=2) if errors else 'None.')+'</pre>'
    document+='<h2>Explicitly unmeasured configurations</h2><pre>'+html.escape(json.dumps(skipped,indent=2) if skipped else 'None.')+'</pre>'
    document+=r'''<script>
const rows=[...document.querySelectorAll('#results tbody tr')], selectors=[];
function filter(){const terms=document.querySelector('#filter').value.toLowerCase().trim().split(/\s+/);let n=0;for(const row of rows){row.hidden=!(selectors.every(([i,s])=>!s.value||row.cells[i].textContent===s.value)&&terms.every(t=>row.textContent.toLowerCase().includes(t)));if(!row.hidden)n++}document.querySelector('#count').textContent=` ${n} / ${rows.length} rows`;}
for(const i of [0,1,2,3,4,6]){const label=document.createElement('label'),s=document.createElement('select');label.textContent=document.querySelectorAll('#results th')[i].textContent;const all=document.createElement('option');all.value='';all.textContent='All';s.append(all);for(const value of [...new Set(rows.map(r=>r.cells[i].textContent))].sort()){const o=document.createElement('option');o.value=o.textContent=value;s.append(o)}label.append(s);document.querySelector('#selectors').append(label);selectors.push([i,s]);s.addEventListener('change',filter);}
document.querySelector('#filter').addEventListener('input',filter);filter();
for(const table of document.querySelectorAll('table')){for(const [i,th] of [...table.querySelectorAll('th')].entries()){const b=document.createElement('button');b.textContent=th.textContent;th.textContent='';th.append(b);let asc=true;b.addEventListener('click',()=>{const rs=[...table.tBodies[0].rows];rs.sort((a,b)=>{const x=a.cells[i].textContent,y=b.cells[i].textContent,nx=Number(x),ny=Number(y);return (Number.isFinite(nx)&&Number.isFinite(ny)?nx-ny:x.localeCompare(y,undefined,{numeric:true}))*(asc?1:-1)});for(const r of rs)table.tBodies[0].append(r);for(const h of table.querySelectorAll('th'))h.removeAttribute('aria-sort');th.setAttribute('aria-sort',asc?'ascending':'descending');asc=!asc;});}}
</script></main></html>'''
    (directory/'index.html').write_text(document)
    print(f'{directory}/index.html: {len(rows)} configurations, {len(errors)} errors')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory')
    generate(parser.parse_args().directory)
