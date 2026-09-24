-- pg_lion_citext: case-insensitive text keys for the lion index AM.
-- citext_hash() and citext's = operator agree on case folding, which is all the
-- AM needs: support proc 1 is the hash, strategy 1 is equality.
--
-- Support proc 4 is the ordering of DESIGN.md §21, and citext_cmp() agrees
-- with both of the above: it returns 0 for two spellings that differ only in
-- case, so 'Alice' and 'alice' tie in the directory and the descent's run scan
-- finds the ONE entry they share.
--
-- Strategies 6 .. 9 are the range comparisons of DESIGN.md §28 (<, <=, >=,
-- >), citext's own, which order by citext_cmp() and so agree with support
-- proc 4: `n < 'M'` walks the directory run between its bounds.
--
-- Every citext object is named through @extschema:citext@.  None of them is in
-- pg_catalog, so unqualified they would be found in this extension's target
-- schema first, where a role with CREATE could plant its own citext_hash() or
-- = and have it run as whoever later inserts into a citext lion index.
\echo Use "CREATE EXTENSION pg_lion_citext" to load this file. \quit
CREATE OPERATOR CLASS citext_ops
    DEFAULT FOR TYPE @extschema:citext@.citext USING lion AS
        OPERATOR 1 @extschema:citext@.= (@extschema:citext@.citext, @extschema:citext@.citext),
        OPERATOR 6 @extschema:citext@.< (@extschema:citext@.citext, @extschema:citext@.citext),
        OPERATOR 7 @extschema:citext@.<= (@extschema:citext@.citext, @extschema:citext@.citext),
        OPERATOR 8 @extschema:citext@.>= (@extschema:citext@.citext, @extschema:citext@.citext),
        OPERATOR 9 @extschema:citext@.> (@extschema:citext@.citext, @extschema:citext@.citext),
        FUNCTION 1 @extschema:citext@.citext_hash(@extschema:citext@.citext),
        FUNCTION 4 @extschema:citext@.citext_cmp(@extschema:citext@.citext, @extschema:citext@.citext);
