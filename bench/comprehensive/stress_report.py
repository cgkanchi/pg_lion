#!/usr/bin/env python3
"""Report the supplemental growth/churn/write measurements."""
import argparse
from collections import defaultdict
import csv
import html
import json
from pathlib import Path
import statistics

from db import nodes
from report import read_jsonl, summary, table, svg_bars


def generate(directory):
    directory=Path(directory)
    meta=json.loads((directory/'metadata.json').read_text())
    records=read_jsonl(directory/'stress.jsonl')
    text=f"""# Growth, churn, dirty memory, and concurrent-write supplement

Commit `{meta['commit']}`. Status: **{meta['status']}**. Server: `{meta.get('server_version','unknown')}`.

This complements the [main comparison](../2026-09-20-comparison/REPORT.md). It uses its own disposable cluster with the same durability, 512 MB shared buffers, default 64 MB work_mem, parallel-query/JIT disabling, and explicit vacuum policy. See [metadata.json](metadata.json) for arguments and settings, and [stress.jsonl](stress.jsonl) for every timing, full query plan, result-check status, and operation record. Families are shuffled with a fixed seed.

**Growth:** {meta['arguments']['rows']:,} rows with unique bigint `k`, two-valued integer `h`, and a 64-character payload; one index on each key. Compare bulk load followed by CREATE INDEX with CREATE INDEX on an empty table followed by the same insert. Insert timing includes index maintenance only in the second route. The sum of recorded load and populated-index build times excludes empty-index DDL and subsequent VACUUM; it is not total settled-construction cost, especially when GIN defers work into its pending list. Vacuum/ANALYZE precedes query timings and size collection, but its duration is not measured in this growth profile. This deliberately exposes fixed initial bucket sizing; it is not the main comparison's eight-index portfolio.

**Churn:** a separate, fixed-size table with the same two keys and no payload starts with a bulk build. Each of {meta['arguments']['cycles']} cycles changes every unique key into a disjoint new range, then vacuums. The row count remains constant, while the historical key domain grows. After the last cycle, REINDEX measures reclaimable storage. Checkpoints precede updates, so their WAL includes full-page images. Observations are one per cycle and index family, not repeated trials of an identical state.

**Dirty memory:** after the bulk-built growth case, updating every payload clears visibility bits across populated pages. Counts/grouping run at 64 kB, 4 MB, and 64 MB work_mem with sequential scans discouraged. This exercises dirty candidates and lossy bitmaps; inspect the actual access route because a planner fallback remains possible. The separate `roaring_sql` diagnostic explicitly calls the direct SQL count function; for the two known groups it unions two such calls. It guarantees exercise of the count engine even when the planner prefers a bitmap scan. This is a specialized equivalent expression for this fixed two-valued dataset, not automatic arbitrary GROUP BY acceleration. Raw recheck statistics verify candidate counts. Every query, including that diagnostic, is compared as an exact result multiset against a forced sequential result before timing.

**Concurrent writes:** a new two-column table and one index on the two-valued `h` column for every client-count point. Closed-loop clients commit 100-row INSERT transactions for {meta['arguments']['duration']} seconds. Durability is enabled; no primary key or unique secondary index is present. The driver uses Python threads and synchronous libpq, so dispatch/parsing/result overhead is included. Every completed transaction is checked against the final row count. Faster methods insert more data during the fixed-duration burst; this is throughput under growth, not equal-sized end states. GIN uses its default fastupdate/pending-list behavior. The final VACUUM/drain time and WAL are reported separately, rather than treating deferred GIN work as free. Each client-count point is one short burst, with no between-run confidence interval; this is not a long-term saturation or replication test.

**Prepared queries:** after the bulk build, equality counts use explicitly typed parameters under `force_custom_plan` and `force_generic_plan`. Plans are prepared fresh after the sequential reference check and warmed before timing. Custom plans can substitute constants; a generic plan retains parameters and can lose roaring count pushdown. These two forced settings bracket the possibilities; the server's automatic custom/generic selection heuristic is not being simulated. Planning and execution times are both retained. The main comparison uses SQL literals.

Query medians, p95, and bootstrap intervals use the main report's method; they describe within-run variation. Build/insert/update/VACUUM durations use end-to-end wall time. No overall speedup is inferred by pooling unlike operations.
"""
    growth_headers=['Family','Load order','Insert ms','Build ms','Recorded load+build ms','Insert WAL MiB','Indexes MiB','Heap MiB']
    growth=[]
    loads={(r['method'],r['order']):r for r in records if r['kind']=='load'}
    builds={(r['method'],r['order']):r for r in records if r['kind']=='build'}
    for r in records:
        if r['kind']=='size' and r['profile']=='growth':
            key=(r['method'],r['order'])
            load=loads[key]
            build=builds.get(key,{}).get('elapsed_ms',0)
            growth.append([*key,f"{load['elapsed_ms']:.3f}",f'{build:.3f}',f"{load['elapsed_ms']+build:.3f}",
                           f"{load['wal_bytes']/2**20:.3f}",f"{r['index_bytes']/2**20:.3f}",f"{r['heap_bytes']/2**20:.3f}"])
    churn_headers=['Family','Cycle / state','Update ms','Update WAL MiB','Vacuum / reindex ms','Indexes MiB','Heap MiB']
    churn=[]
    operations={(r['method'],r.get('cycle'),r['kind']):r for r in records if r['kind'] in ['update','vacuum','reindex']}
    for r in records:
        if r['kind']=='size' and r['profile'] in ['churn','after_reindex']:
            key=(r['method'],r['cycle'])
            update=operations.get(key+('update',),{}) if r['profile']=='churn' else {}
            cleanup=operations.get(key+(('vacuum' if r['profile']=='churn' else 'reindex'),),{})
            churn.append([r['method'],r['cycle'] if r['profile']=='churn' else 'after REINDEX',
                          f"{update.get('elapsed_ms',0):.3f}",f"{update.get('wal_bytes',0)/2**20:.3f}",
                          f"{cleanup.get('elapsed_ms',0):.3f}",f"{r['index_bytes']/2**20:.3f}",f"{r['heap_bytes']/2**20:.3f}"])
    query_headers=['Profile','Variant','State','Query','n','Execution median ms','95% CI ms','p95 ms','Access','Lossy heap blocks (max)','Planning median ms','Plan+execution median ms']
    query_rows=[]
    groups=defaultdict(list)
    for r in records:
        if r['kind']=='query':
            state=r.get('order',r.get('memory',r.get('plan_mode',f"cycle {r.get('cycle')}")))
            groups[(r['profile'],r['variant'],state,r['query'])].append(r)
    for key,values in sorted(groups.items()):
        st=summary([r['execution_ms'] for r in values])
        routes=set()
        for r in values:
            for n in nodes(r['plan']['Plan']):
                if 'Scan' in n['Node Type']:
                    routes.add(n.get('Custom Plan Provider',n['Node Type']))
        if key[1]=='roaring_sql':
            routes.add('direct SQL count')
        lossy=max(sum(n.get('Lossy Heap Blocks',0) for n in nodes(r['plan']['Plan'])) for r in values)
        query_rows.append([*key,st['n'],f"{st['median']:.3f}",f"{st['lo']:.3f}–{st['hi']:.3f}",f"{st['p95']:.3f}",', '.join(sorted(routes)),lossy,
                           f"{statistics.median(r['plan']['Planning Time'] for r in values):.3f}",
                           f"{statistics.median(r['plan']['Planning Time']+r['execution_ms'] for r in values):.3f}"])
    writer_headers=['Family','Clients','Rows inserted','Transactions/s','Median ms','p95 ms','WAL MiB','Indexes MiB','Drain ms','Drain WAL MiB']
    writers=[]
    drain={(r['method'],r['clients']):r for r in records if r['kind']=='writer_vacuum'}
    for r in records:
        if r['kind']=='writers':
            d=drain[(r['method'],r['clients'])]
            writers.append([r['method'],r['clients'],r['rows'],f"{r['tps']:.1f}",f"{r['median_ms']:.3f}",f"{r['p95_ms']:.3f}",
                            f"{r['wal_bytes']/2**20:.3f}",f"{r['index_bytes']/2**20:.3f}",f"{d['elapsed_ms']:.3f}",f"{d['wal_bytes']/2**20:.3f}"])
    stats_headers=['Profile','State','Index','Height','Leaf pages','Entries','Container pages','TIDs']
    stats_rows=[]
    for r in records:
        if r['kind']=='roaring_stats':
            stats_rows.append([r['profile'],r.get('order',f"cycle {r.get('cycle')}"),r['index'],
                               r.get('directory_height',r.get('nbuckets')),
                               r.get('leaf_pages',r.get('bucket_pages')),
                               r['entries'],r['container_pages'],r['ntids']])
    tables=[('Construction routes',growth_headers,growth),('Changing-key churn',churn_headers,churn),
            ('Concurrent inserts',writer_headers,writers),('Roaring storage structure',stats_headers,stats_rows),
            ('Every measured query',query_headers,query_rows)]
    for name,headers,rows in tables:
        filename=name.lower().replace(' ','-')+'.csv'
        with (directory/filename).open('w',newline='') as stream:
            writer=csv.writer(stream)
            writer.writerow(headers)
            writer.writerows(rows)
    charts=[]
    for profile in ['growth','churn','after_reindex']:
        values=[(r['method'],r['index_bytes']/2**20) for r in records if r['kind']=='size' and r['profile']==profile
                and (profile!='growth' or r['order']=='index_then_insert')
                and (profile!='churn' or r['cycle']==meta['arguments']['cycles'])]
        name=profile+'-size.svg'
        title=profile.replace('_',' ')+' index size (lower is smaller)'
        svg=svg_bars(title,sorted(values,key=lambda x:x[1]),'MiB')
        (directory/name).write_text(svg)
        charts.append((title,name,svg))
    checks=sum(r['kind']=='correctness' for r in records)
    text+=f'\n**{checks} successful query result checks; {sum(r["kind"]=="query" for r in records):,} timed queries.**\n'
    text+='\n'+''.join(f'![{title}]({name})\n\n' for title,name,_ in charts)
    for title,headers,rows in tables:
        filename=title.lower().replace(' ','-')+'.csv'
        text+='\n## '+title+'\n\n'+f'[Download CSV]({filename})\n\n'+table(headers,rows)
    (directory/'REPORT.md').write_text(text)
    doc='<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Index growth and write stress</title><style>body{font:15px/1.5 system-ui;margin:30px;color:#173047}table{border-collapse:collapse;font-size:13px}th,td{padding:8px;border-bottom:1px solid #ddd;text-align:left;white-space:nowrap}th{background:#173047;color:white;position:sticky;top:0}.scroll{overflow:auto;max-height:650px}svg{max-width:850px;width:100%}pre{white-space:pre-wrap;max-width:1100px}h1,h2{line-height:1.2}</style><h1>Index growth, churn, and concurrent writes</h1><p><a href="REPORT.md">Full report and methodology</a> · <a href="stress.jsonl">All raw records and plans</a> · <a href="../2026-09-20-comparison/index.html">Main comparison</a></p>'
    doc+=f'<p>Status: {html.escape(meta["status"])}. {checks} successful query checks. Commit {html.escape(meta["commit"])}.</p>'
    doc+=''.join(svg for _,_,svg in charts)
    for title,headers,rows in tables:
        filename=title.lower().replace(' ','-')+'.csv'
        doc+='<h2>'+html.escape(title)+'</h2><p><a href="'+filename+'">Download CSV</a></p><div class="scroll"><table><thead><tr>'+''.join('<th>'+html.escape(str(x))+'</th>' for x in headers)+'</tr></thead><tbody>'
        doc+=''.join('<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in row)+'</tr>' for row in rows)+'</tbody></table></div>'
    doc+='</html>'
    (directory/'index.html').write_text(doc)
    print(directory/'index.html')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory')
    generate(parser.parse_args().directory)
