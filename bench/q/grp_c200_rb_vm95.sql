select r.key, rb_and_cardinality(r.idx2048, m.mask_95)
       + (select count(*) from fact f where f.ctid = any(array(select int8_to_tid(x) from rb_iterate(rb_andnot(r.idx2048, m.mask_95)) x)))
from rb_c200 r, vm_mask m;
