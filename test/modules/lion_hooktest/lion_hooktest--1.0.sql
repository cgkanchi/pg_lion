\echo Use "CREATE EXTENSION lion_hooktest" to load this file. \quit

CREATE FUNCTION lion_hooktest_calls() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION lion_hooktest_saw_lion() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION lion_hooktest_reset() RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION lion_hooktest_rel_calls() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION lion_hooktest_rel_saw_ordered() RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION lion_hooktest_heapcopy_handler(internal) RETURNS table_am_handler
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- A table AM that stores heap tuples but is not the heap as far as pg_lion
-- can tell (lion_table_am_supported()).
CREATE ACCESS METHOD lion_heapcopy TYPE TABLE HANDLER lion_hooktest_heapcopy_handler;
