\timing on
set maintenance_work_mem = '1GB';
drop table if exists fact_w;
create table fact_w as select * from fact limit 2000000;
vacuum analyze fact_w;
create or replace function newrows(n int) returns setof fact language sql volatile as $$
  select (random()<0.5)::int, floor(random()*10)::int, floor(random()*200)::int, floor(random()*20000)::int,
         floor(random()*1000000)::int, floor(random()*200)::int,
         case when random()<0.9 then 0 else 1+floor(random()*999)::int end, md5(random()::text)
  from generate_series(1,n) $$;
\echo === baseline: no indexes: insert 200k / update ~20k
insert into fact_w select * from newrows(200000);
update fact_w set c10 = (c10+1) % 10 where c1m % 100 = 0;
\echo === 7 btree indexes
create index on fact_w using btree (c2); create index on fact_w using btree (c10); create index on fact_w using btree (c200);
create index on fact_w using btree (c20k); create index on fact_w using btree (c1m); create index on fact_w using btree (c200_clustered);
create index on fact_w using btree (c_skew);
\echo --- btree: insert 200k
insert into fact_w select * from newrows(200000);
\echo --- btree: update ~20k
update fact_w set c10 = (c10+1) % 10 where c1m % 100 = 0;
select format('drop index %I', indexrelid::regclass) from pg_index where indrelid = 'fact_w'::regclass \gexec
\echo === 7 GIN indexes, fastupdate=off
create index on fact_w using gin (c2) with (fastupdate=off); create index on fact_w using gin (c10) with (fastupdate=off); create index on fact_w using gin (c200) with (fastupdate=off);
create index on fact_w using gin (c20k) with (fastupdate=off); create index on fact_w using gin (c1m) with (fastupdate=off); create index on fact_w using gin (c200_clustered) with (fastupdate=off);
create index on fact_w using gin (c_skew) with (fastupdate=off);
\echo --- gin(fastupdate=off): insert 200k
insert into fact_w select * from newrows(200000);
\echo --- gin(fastupdate=off): update ~20k
update fact_w set c10 = (c10+1) % 10 where c1m % 100 = 0;
select format('alter index %I set (fastupdate=on)', indexrelid::regclass) from pg_index where indrelid = 'fact_w'::regclass \gexec
\echo --- gin(fastupdate=on): insert 200k
insert into fact_w select * from newrows(200000);
\echo --- gin(fastupdate=on): update ~20k
update fact_w set c10 = (c10+1) % 10 where c1m % 100 = 0;
