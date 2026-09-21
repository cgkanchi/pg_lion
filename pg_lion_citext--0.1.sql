-- pg_lion_citext: case-insensitive text keys for the lion index AM.
-- citext_hash() and citext's = operator agree on case folding, which is all the
-- AM needs: support proc 1 is the hash, strategy 1 is equality.
\echo Use "CREATE EXTENSION pg_lion_citext" to load this file. \quit
CREATE OPERATOR CLASS citext_ops
    DEFAULT FOR TYPE citext USING lion AS
        OPERATOR 1 = (citext, citext),
        FUNCTION 1 citext_hash(citext);
