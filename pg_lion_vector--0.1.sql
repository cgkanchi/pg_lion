-- pg_lion_vector: the lionvec index access method for pgvector's `vector`
-- and `halfvec` (DESIGN.md §44).  The AM is in pg_lion's library, which
-- reads pgvector's on-disk formats itself, so pg_lion loads without
-- pgvector; this extension is what creates the AM, where pgvector is
-- installed.
--
-- Each operator class is one ORDER BY operator, which also tells the index
-- its metric: strategy 1 is L2 distance (<->), 2 negative inner product
-- (<#>), 3 cosine distance (<=>).  The scan computes the distances it
-- returns with that operator's own function, so the values are pgvector's.
-- An index reads its column as a halfvec when its operator class's type is
-- named halfvec, and as a vector otherwise.
--
-- Every pgvector object is named through @extschema:vector@, so that none of
-- them can be shadowed by an object planted in this extension's schema.
\echo Use "CREATE EXTENSION pg_lion_vector" to load this file. \quit

CREATE FUNCTION lionvec_handler(internal) RETURNS index_am_handler
    AS '$libdir/pg_lion', 'lionvec_handler' LANGUAGE C;

CREATE ACCESS METHOD lionvec TYPE INDEX HANDLER lionvec_handler;
COMMENT ON ACCESS METHOD lionvec IS 'nearest-neighbour index of bit-plane codes, filtered by lion';

CREATE OPERATOR CLASS vector_l2_ops
    DEFAULT FOR TYPE @extschema:vector@.vector USING lionvec AS
        OPERATOR 1 @extschema:vector@.<-> (@extschema:vector@.vector, @extschema:vector@.vector)
            FOR ORDER BY pg_catalog.float_ops;

CREATE OPERATOR CLASS vector_ip_ops
    FOR TYPE @extschema:vector@.vector USING lionvec AS
        OPERATOR 2 @extschema:vector@.<#> (@extschema:vector@.vector, @extschema:vector@.vector)
            FOR ORDER BY pg_catalog.float_ops;

CREATE OPERATOR CLASS vector_cosine_ops
    FOR TYPE @extschema:vector@.vector USING lionvec AS
        OPERATOR 3 @extschema:vector@.<=> (@extschema:vector@.vector, @extschema:vector@.vector)
            FOR ORDER BY pg_catalog.float_ops;

CREATE OPERATOR CLASS halfvec_l2_ops
    DEFAULT FOR TYPE @extschema:vector@.halfvec USING lionvec AS
        OPERATOR 1 @extschema:vector@.<-> (@extschema:vector@.halfvec, @extschema:vector@.halfvec)
            FOR ORDER BY pg_catalog.float_ops;

CREATE OPERATOR CLASS halfvec_ip_ops
    FOR TYPE @extschema:vector@.halfvec USING lionvec AS
        OPERATOR 2 @extschema:vector@.<#> (@extschema:vector@.halfvec, @extschema:vector@.halfvec)
            FOR ORDER BY pg_catalog.float_ops;

CREATE OPERATOR CLASS halfvec_cosine_ops
    FOR TYPE @extschema:vector@.halfvec USING lionvec AS
        OPERATOR 3 @extschema:vector@.<=> (@extschema:vector@.halfvec, @extschema:vector@.halfvec)
            FOR ORDER BY pg_catalog.float_ops;
