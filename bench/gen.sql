\timing on
create extension if not exists btree_gin;
create extension if not exists pg_prewarm;
drop table if exists fact cascade;
create table fact as
select
  (random() < 0.5)::int                          as c2,
  floor(random()*10)::int                        as c10,
  floor(random()*200)::int                       as c200,
  floor(random()*20000)::int                     as c20k,
  floor(random()*1000000)::int                   as c1m,
  (g / 100000)::int                              as c200_clustered,
  case when random() < 0.9 then 0 else 1 + floor(random()*999)::int end as c_skew,
  md5(g::text)                                   as payload
from generate_series(1, 20000000) g;
vacuum (analyze, freeze) fact;
select pg_size_pretty(pg_relation_size('fact')) as heap_size, pg_relation_size('fact')/8192 as heap_pages;
create index fact_c2_btree on fact using btree (c2);
create index fact_c10_btree on fact using btree (c10);
create index fact_c200_btree on fact using btree (c200);
create index fact_c20k_btree on fact using btree (c20k);
create index fact_c1m_btree on fact using btree (c1m);
create index fact_c200_clustered_btree on fact using btree (c200_clustered);
create index fact_c_skew_btree on fact using btree (c_skew);
create index fact_c2_gin on fact using gin (c2);
create index fact_c10_gin on fact using gin (c10);
create index fact_c200_gin on fact using gin (c200);
create index fact_c20k_gin on fact using gin (c20k);
create index fact_c1m_gin on fact using gin (c1m);
create index fact_c200_clustered_gin on fact using gin (c200_clustered);
create index fact_c_skew_gin on fact using gin (c_skew);
drop table if exists fact_tids;
create unlogged table fact_tids as
select ctid as t,
       (ctid::text::point)[0]::bigint as blk,
       (ctid::text::point)[1]::bigint as off,
       c2, c10, c200, c20k, c1m, c200_clustered, c_skew
from fact;
select max(off) as max_offset, max(blk) as max_block, count(*) from fact_tids;
select relname, pg_size_pretty(pg_relation_size(oid)) from pg_class where relname like 'fact%' order by relname;
