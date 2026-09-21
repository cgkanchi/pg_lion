select (select rb_and_cardinality(r.idx2048, m.mask_95) from rb_c20k r, vm_mask m where r.key = 123)
     + (select count(*) from fact where ctid = any(array(select int8_to_tid(x) from rb_c20k r, vm_mask m, rb_iterate(rb_andnot(r.idx2048, m.mask_95)) x where r.key = 123)));
