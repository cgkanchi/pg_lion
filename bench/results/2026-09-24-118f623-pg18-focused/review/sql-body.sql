-- Run in a fresh disposable database with pg_lion installed.
\set ON_ERROR_STOP on
CREATE EXTENSION pg_lion;
CREATE FUNCTION lion_ocmp(int4, int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT
	RETURN pg_catalog.btint4cmp($1, $2);
CREATE OPERATOR FAMILY lion_ofam USING lion;
CREATE OPERATOR CLASS lion_oops FOR TYPE int4 USING lion FAMILY lion_ofam AS
	OPERATOR 1 = (int4, int4),
	OPERATOR 6 < (int4, int4),
	OPERATOR 7 <= (int4, int4),
	OPERATOR 8 >= (int4, int4),
	OPERATOR 9 > (int4, int4),
	FUNCTION 1 hashint4(int4),
	FUNCTION 4 lion_ocmp(int4, int4);

-- operators of our own, which a btree opclass over lion_ocmp can name
CREATE FUNCTION lion_olt(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 < $2';
CREATE FUNCTION lion_ole(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 <= $2';
CREATE FUNCTION lion_oeq(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 = $2';
CREATE FUNCTION lion_oge(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 >= $2';
CREATE FUNCTION lion_ogt(int4, int4) RETURNS bool LANGUAGE sql IMMUTABLE STRICT AS 'SELECT $1 > $2';
CREATE OPERATOR <<< (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_olt);
CREATE OPERATOR <<= (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ole);
CREATE OPERATOR === (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_oeq);
CREATE OPERATOR >>= (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_oge);
CREATE OPERATOR >>> (LEFTARG = int4, RIGHTARG = int4, FUNCTION = lion_ogt);


CREATE OPERATOR CLASS lion_obt FOR TYPE int4 USING btree AS
	OPERATOR 1 <<<,
	OPERATOR 2 <<=,
	OPERATOR 3 ===,
	OPERATOR 4 >>=,
	OPERATOR 5 >>>,
	FUNCTION 1 lion_ocmp(int4, int4);
CREATE TABLE identity_probe(k int);
INSERT INTO identity_probe SELECT i%5000 FROM generate_series(1,50000)i;
CREATE INDEX identity_idx ON identity_probe USING lion(k lion_oops);
VACUUM (FREEZE, ANALYZE) identity_probe;
SELECT lion_index_count('identity_idx',7); -- 10
CREATE OR REPLACE FUNCTION lion_ocmp(int4,int4) RETURNS int4 LANGUAGE sql IMMUTABLE STRICT RETURN pg_catalog.btint4cmp($2,$1);
\connect

SELECT lion_index_count('identity_idx',7); -- BUG: 0, expected rejection requiring REINDEX
SET pg_lion.enable_count_pushdown=off;
SET enable_bitmapscan=off;
SELECT count(*) FROM identity_probe WHERE k=7; -- 10
