-- roaring_index_citext: case-insensitive text keys for the roaring index AM.
-- citext_hash() and citext's = operator agree on case folding, which is all the
-- AM needs: support proc 1 is the hash, strategy 1 is equality.
\echo Use "CREATE EXTENSION roaring_index_citext" to load this file. \quit
CREATE OPERATOR CLASS citext_ops
    DEFAULT FOR TYPE citext USING roaring AS
        OPERATOR 1 = (citext, citext),
        FUNCTION 1 citext_hash(citext);
