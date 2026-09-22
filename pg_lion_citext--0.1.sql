-- pg_lion_citext: case-insensitive text keys for the lion index AM.
-- citext_hash() and citext's = operator agree on case folding, which is all the
-- AM needs: support proc 1 is the hash, strategy 1 is equality.
--
-- Support proc 4 is the ordering of DESIGN.md §21, and citext_cmp() agrees
-- with both of the above: it returns 0 for two spellings that differ only in
-- case, so 'Alice' and 'alice' tie in the directory and the descent's run scan
-- finds the ONE entry they share.
\echo Use "CREATE EXTENSION pg_lion_citext" to load this file. \quit
CREATE OPERATOR CLASS citext_ops
    DEFAULT FOR TYPE citext USING lion AS
        OPERATOR 1 = (citext, citext),
        FUNCTION 1 citext_hash(citext),
        FUNCTION 4 citext_cmp(citext, citext);
