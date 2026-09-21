with r as (select rb_and(rb_and(a.idx2048, b.idx2048), c.idx2048) as bm from rb_c10 a, rb_c200 b, rb_c2 c where a.key = 3 and b.key = 17 and c.key = 1)
select (select rb_and_cardinality(r.bm, m.mask_95) from r, vm_mask m)
     + (select count(*) from fact where ctid = any(array(select int8_to_tid(x) from r, vm_mask m, rb_iterate(rb_andnot(r.bm, m.mask_95)) x)));
