create function int8_to_tid(bigint) returns tid as 'MODULE_PATHNAME' language c immutable strict parallel safe;
create function tid_to_int8(tid) returns bigint as 'MODULE_PATHNAME' language c immutable strict parallel safe;
