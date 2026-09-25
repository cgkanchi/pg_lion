from pathlib import Path
import sys,json,itertools,random
s=json.loads(Path('/tmp/lion-range-review-state.json').read_text());base=Path(s['base']);info=s['versions']['20'];sys.path.insert(0,str(Path(info['src'])/'bench/comprehensive'))
from run import Cluster
from db import digest
out=base/'adversarial';out.mkdir();c=Cluster(info['prefix'],out,preload=True);results=[]
try:
 c.start(initialize=True);db=c.db;db.query('CREATE EXTENSION pg_lion')
 sql=(Path(info['src'])/'test/sql/ordering.sql').read_text();start=sql.index('CREATE FUNCTION lion_ocmp(');end=sql.index('CREATE TABLE lion_ord (')
 setup=sql[start:end].replace("AS 'SELECT btint4cmp($1, $2)'",'RETURN pg_catalog.btint4cmp($1, $2)')
 db.query(setup)
 start=sql.index('CREATE OPERATOR CLASS lion_obt');end=sql.index(';',start)+1;db.query(sql[start:end])
 db.query('CREATE TABLE identity_probe(k int); INSERT INTO identity_probe SELECT i%5000 FROM generate_series(1,50000)i; CREATE INDEX identity_idx ON identity_probe USING lion(k lion_oops)');db.query('VACUUM(FREEZE,ANALYZE) identity_probe')
 for q in ["SELECT prosrc, prosqlbody IS NOT NULL FROM pg_proc WHERE oid='lion_ocmp(int,int)'::regprocedure", "SELECT ordered FROM lion_index_stats('identity_idx')", "SELECT lion_index_count('identity_idx',7)"]:
  results.append(dict(sql=q,result=db.query(q)))
 db.query('CREATE OR REPLACE FUNCTION lion_ocmp(int4,int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT RETURN pg_catalog.btint4cmp($2,$1)')
 db.close();c.db=None;c.connect();db=c.db
 for q in ["SELECT prosrc, prosqlbody IS NOT NULL FROM pg_proc WHERE oid='lion_ocmp(int,int)'::regprocedure", "SELECT ordered FROM lion_index_stats('identity_idx')", "SELECT lion_index_count('identity_idx',7)", "SET pg_lion.enable_count_pushdown=off;SET enable_bitmapscan=off;SELECT count(*) FROM identity_probe WHERE k=7"]:
  try:r=dict(sql=q,result=db.query(q))
  except Exception as e:r=dict(sql=q,error=str(e))
  results.append(r)
 print('IDENTITY',json.dumps(results),flush=True);(out/'order-identity.json').write_text(json.dumps(results,indent=2))
 db.query('DROP TABLE identity_probe;DROP OPERATOR CLASS lion_obt USING btree;DROP OPERATOR CLASS lion_oops USING lion')
 db.query('CREATE TABLE fuzz(id int,k int,j int,x float8)');db.query("INSERT INTO fuzz SELECT i,CASE WHEN i%17=0 THEN NULL ELSE i%101-50 END,CASE WHEN i%19=0 THEN NULL ELSE i%13 END,CASE WHEN i%23=0 THEN 'NaN'::float8 ELSE (i%101-50)::float8 END FROM generate_series(1,12000)i")
 for col in ['k','j','x']:db.query(f'CREATE INDEX ON fuzz USING lion({col})')
 db.query('CREATE INDEX ON fuzz USING lion(k,j)');db.query('VACUUM(FREEZE,ANALYZE) fuzz')
 conds=['k BETWEEN -10 AND 20','k>20 AND k< -10','k< -49','k>=50','k < ANY(ARRAY[-5,8,NULL])','k >= ANY(ARRAY[-5,8,NULL]::bigint[])','k < ANY(ARRAY[]::int[])','k >= ALL(ARRAY[-5,8,NULL])',"x >= 'NaN'::float8",'k> -10 AND j<7','k> -10 AND j IN(1,3,NULL)','(k< -20 OR k>20) AND j>4','k BETWEEN -9 AND 9 AND (j=2 OR j IS NULL)','k> -9 AND k>0 AND k<=9 AND k<8','k< 10000000000::bigint','k> -10000000000::bigint']
 queries=[f'SELECT {sel} FROM fuzz WHERE {cond}{grp}' for cond in conds for sel,grp in [('count(*)',''),('count(k),count(DISTINCT k)',''),('k,count(*)',' GROUP BY k'),('j,count(*)',' GROUP BY j'),('k,count(DISTINCT j)',' GROUP BY k'),('id','')]]
 failures=[];checks=0;pushed=0
 for phase in ['clean','dirty']:
  if phase=='dirty':db.query('UPDATE fuzz SET k=k+3,j=NULL WHERE id%7=0;DELETE FROM fuzz WHERE id%11=0')
  for memory in ['64MB','64kB']:
   for q in queries:
    db.query(f"SET work_mem='{memory}';SET pg_lion.enable_count_pushdown=off;SET enable_seqscan=on;SET enable_bitmapscan=off;SET enable_indexscan=off;SET enable_indexonlyscan=off")
    ref=db.query(q)
    for mode in ['on','off']:
     db.query(f'SET pg_lion.enable_count_pushdown={mode};SET enable_seqscan=off;SET enable_bitmapscan=on')
     plan=db.explain(q,analyze=False);got=db.query(q);pushed+='LionCount' in json.dumps(plan);checks+=1
     if digest(ref)!=digest(got):failures.append(dict(q=q,phase=phase,memory=memory,mode=mode,ref=ref[:10],got=got[:10],plan=plan))
 print('RANGE FUZZ',checks,'checks',pushed,'pushed',len(failures),'failures',flush=True)
 (out/'fuzz.json').write_text(json.dumps(dict(checks=checks,pushed=pushed,failures=failures),indent=2))
finally:c.close()
