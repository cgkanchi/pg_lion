"""Small synchronous libpq adapter; no third-party Python driver required."""
import ctypes as C
import hashlib
import json


class DatabaseError(RuntimeError):
    pass


class DB:
    def __init__(self, libpq, conninfo):
        self.pq = C.CDLL(str(libpq))
        signatures = {
            'PQconnectdb': (C.c_void_p, [C.c_char_p]),
            'PQstatus': (C.c_int, [C.c_void_p]),
            'PQerrorMessage': (C.c_char_p, [C.c_void_p]),
            'PQexec': (C.c_void_p, [C.c_void_p, C.c_char_p]),
            'PQresultStatus': (C.c_int, [C.c_void_p]),
            'PQresultErrorMessage': (C.c_char_p, [C.c_void_p]),
            'PQntuples': (C.c_int, [C.c_void_p]),
            'PQnfields': (C.c_int, [C.c_void_p]),
            'PQfname': (C.c_char_p, [C.c_void_p, C.c_int]),
            'PQgetisnull': (C.c_int, [C.c_void_p, C.c_int, C.c_int]),
            'PQgetvalue': (C.c_char_p, [C.c_void_p, C.c_int, C.c_int]),
            'PQclear': (None, [C.c_void_p]),
            'PQfinish': (None, [C.c_void_p]),
        }
        for name, (ret, args) in signatures.items():
            fn = getattr(self.pq, name)
            fn.restype, fn.argtypes = ret, args
        self.conn = self.pq.PQconnectdb(conninfo.encode())
        if not self.conn or self.pq.PQstatus(self.conn):
            message = self.pq.PQerrorMessage(self.conn).decode()
            self.close()
            raise DatabaseError(message)

    def close(self):
        if self.conn:
            self.pq.PQfinish(self.conn)
            self.conn = None

    def query(self, sql, dictionaries=False):
        result = self.pq.PQexec(self.conn, sql.encode())
        if not result:
            raise DatabaseError(self.pq.PQerrorMessage(self.conn).decode())
        try:
            if self.pq.PQresultStatus(result) not in (1, 2):
                raise DatabaseError(self.pq.PQresultErrorMessage(result).decode())
            nf = self.pq.PQnfields(result)
            rows = [
                [None if self.pq.PQgetisnull(result, i, j)
                 else self.pq.PQgetvalue(result, i, j).decode()
                 for j in range(nf)]
                for i in range(self.pq.PQntuples(result))
            ]
            if dictionaries:
                names = [self.pq.PQfname(result, j).decode() for j in range(nf)]
                return [dict(zip(names, row)) for row in rows]
            return rows
        finally:
            self.pq.PQclear(result)

    def scalar(self, sql):
        return self.query(sql)[0][0]

    def explain(self, sql, analyze=True):
        options = 'ANALYZE, BUFFERS, WAL, TIMING OFF, SUMMARY ON, FORMAT JSON' if analyze else 'FORMAT JSON'
        return json.loads(self.scalar(f'EXPLAIN ({options}) {sql}'))[0]


def digest(rows):
    """Exact result multiset, including duplicate rows and SQL NULLs."""
    canonical = sorted(json.dumps(r, ensure_ascii=False, separators=(',', ':')) for r in rows)
    return hashlib.sha256('\n'.join(canonical).encode()).hexdigest()


def nodes(plan):
    yield plan
    for child in plan.get('Plans', []):
        yield from nodes(child)
