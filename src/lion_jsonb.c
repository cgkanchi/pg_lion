/*-------------------------------------------------------------------------
 *
 * lion_jsonb.c
 *		jsonb_contains_ops: path keys for jsonb `@>`, `?`, `?|`, `?&`, and jsonpath
 *		`@?` and `@@` (DESIGN.md §42, §43).
 *
 * The operator class indexes a jsonb document under these keys, each a bytea
 * that starts with a tag byte:
 *
 *	'R' + 'o' | 'a'		the document is an object | an array (a raw scalar
 *						has no root key)
 *	'L' path value		a scalar at a path
 *	'C' path type		a container ('o' | 'a') at a path below the root
 *	'E' name			a top-level object key, a string element of a
 *						top-level array, or a raw scalar string: what `?`
 *						tests
 *	'H' sha256			any of the above that is too long to be a key
 *
 * A path is the object key names from the root, then the value, then how many
 * array levels lie before each name and after the last one.  Array indexes
 * are not part of a path: `@>` matches an array element wherever it is.
 * A raw scalar document gets the key a scalar element of a top-level array
 * would get, which is how `'["foo"]' @> '"foo"'` and `'"foo"' @> '"foo"'`
 * both find it.  Numbers are written by numeric_normalize(), so that 1, 1.0
 * and 1e0 give one key; strings are their bytes, which is what jsonb equality
 * compares.
 *
 * The names come before the value and the array levels after it so that one
 * run of the directory holds every array nesting of the same `path = value`.
 *
 * `@>` is the AND of the query's keys: its leaves, a container key for each
 * EMPTY container in it, and the root key when the query is an array or an
 * empty object (a non-empty object query's leaves already say the document
 * is an object; an array query must not match a raw scalar).  That AND is
 * exact unless one array element in the query contributes two or more keys,
 * because only then must two keys be found in the SAME element of the
 * document; such a query, and one with a hashed key, is returned with
 * LION_SEARCH_MODE_LOSSY and every row the AND selects is rechecked.
 *
 * `?` is one existence key, `?|` their OR and `?&` their AND, NULL elements
 * skipped as PostgreSQL skips them.  `?& '{}'` is true for every document,
 * which no key says, so it asks for every row (GIN_SEARCH_MODE_ALL).
 *
 * `@?` and `@@` with a jsonpath (DESIGN.md §43) are always a superset and
 * rechecked.  The query is a tree of AND and OR over key PREFIXES, which the
 * executor expands into the index's keys (lion_extract_query_superset()):
 * `path == scalar` is the leaf keys that start with the path and the value,
 * at any array levels, and a path that must exist is the leaf and container
 * keys under it.  See lion_jp_bool() for what is taken apart.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/numeric.h"
#include "varatt.h"

#include "lion.h"

#define LION_JK_ROOT		'R'
#define LION_JK_LEAF		'L'
#define LION_JK_CONTAINER	'C'
#define LION_JK_EXISTS		'E'
#define LION_JK_HASHED		'H'

#define LION_JK_STEP		0x01	/* an object key name follows */
#define LION_JK_ENDPATH		0x00	/* the names are done */

/*
 * The path to where the walk is: the object key names from the root, and
 * marks[i] array levels between name i - 1 and name i (marks[0] before the
 * first name, marks[nnames] after the last).
 */
typedef struct LionJsonbPath
{
	int			nnames;
	int			maxnames;
	const char **name;
	int		   *namelen;
	int		   *marks;			/* nnames + 1 of them */
} LionJsonbPath;

/* One container the walk is inside. */
typedef struct LionJsonbFrame
{
	bool		isarray;
	bool		named;			/* entered under an object key: pop the name */
	bool		element;		/* an element of an array */
	int			firstkey;		/* the keys emitted before it began */
} LionJsonbFrame;

typedef struct LionJsonbKeys
{
	Datum	   *keys;
	int			nkeys;
	int			maxkeys;
	bool		hashed;			/* some key was too long and was hashed */
} LionJsonbKeys;

static void
lion_jsonb_put_uvarint(StringInfo buf, uint32 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(buf, (char) ((v & 0x7f) | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(buf, (char) v);
}

static void
lion_jsonb_put_bytes(StringInfo buf, const char *s, int len)
{
	lion_jsonb_put_uvarint(buf, (uint32) len);
	appendBinaryStringInfo(buf, s, len);
}

/*
 * Turn the bytes in buf into a key, hashing them when they are too long for
 * one (LION_MAX_KEY_SIZE): the hash keeps the key a superset of the rows it
 * names, and a query that needs one rechecks.
 */
static void
lion_jsonb_emit(LionJsonbKeys *out, StringInfo buf)
{
	bytea	   *key;

	if (VARHDRSZ + buf->len > LION_MAX_KEY_SIZE)
	{
		pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
		uint8		digest[PG_SHA256_DIGEST_LENGTH];

		if (pg_cryptohash_init(ctx) < 0 ||
			pg_cryptohash_update(ctx, (const uint8 *) buf->data, buf->len) < 0 ||
			pg_cryptohash_final(ctx, digest, sizeof(digest)) < 0)
			elog(ERROR, "could not compute a SHA-256 digest: %s",
				 pg_cryptohash_error(ctx));
		pg_cryptohash_free(ctx);

		key = (bytea *) palloc(VARHDRSZ + 1 + PG_SHA256_DIGEST_LENGTH);
		SET_VARSIZE(key, VARHDRSZ + 1 + PG_SHA256_DIGEST_LENGTH);
		VARDATA(key)[0] = LION_JK_HASHED;
		memcpy(VARDATA(key) + 1, digest, PG_SHA256_DIGEST_LENGTH);
		out->hashed = true;
	}
	else
	{
		key = (bytea *) palloc(VARHDRSZ + buf->len);
		SET_VARSIZE(key, VARHDRSZ + buf->len);
		memcpy(VARDATA(key), buf->data, buf->len);
	}

	if (out->nkeys >= out->maxkeys)
	{
		out->maxkeys = out->maxkeys * 2 + 16;
		out->keys = out->keys == NULL ?
			(Datum *) palloc(sizeof(Datum) * out->maxkeys) :
			(Datum *) repalloc(out->keys, sizeof(Datum) * out->maxkeys);
	}
	out->keys[out->nkeys++] = PointerGetDatum(key);
}

/*
 * Write the path's names, with one more name (extra) when it is not NULL, and
 * leave the array levels for lion_jsonb_put_marks() to write after the value.
 */
static void
lion_jsonb_put_names(StringInfo buf, const LionJsonbPath *path,
					 const char *extra, int extralen)
{
	int			i;

	for (i = 0; i < path->nnames; i++)
	{
		appendStringInfoChar(buf, LION_JK_STEP);
		lion_jsonb_put_bytes(buf, path->name[i], path->namelen[i]);
	}
	if (extra != NULL)
	{
		appendStringInfoChar(buf, LION_JK_STEP);
		lion_jsonb_put_bytes(buf, extra, extralen);
	}
	appendStringInfoChar(buf, LION_JK_ENDPATH);
}

static void
lion_jsonb_put_marks(StringInfo buf, const LionJsonbPath *path, bool extra)
{
	int			i;

	for (i = 0; i <= path->nnames; i++)
		lion_jsonb_put_uvarint(buf, (uint32) path->marks[i]);
	if (extra)
		lion_jsonb_put_uvarint(buf, 0);
}

static void
lion_jsonb_put_scalar(StringInfo buf, const JsonbValue *v)
{
	switch (v->type)
	{
		case jbvNull:
			appendStringInfoChar(buf, 'z');
			break;
		case jbvBool:
			appendStringInfoChar(buf, v->val.boolean ? 't' : 'f');
			break;
		case jbvNumeric:
			{
				char	   *s = numeric_normalize(v->val.numeric);

				appendStringInfoChar(buf, 'n');
				lion_jsonb_put_bytes(buf, s, (int) strlen(s));
				pfree(s);
				break;
			}
		case jbvString:
			appendStringInfoChar(buf, 's');
			lion_jsonb_put_bytes(buf, v->val.string.val, v->val.string.len);
			break;
		default:
			elog(ERROR, "unexpected jsonb value type %d", (int) v->type);
	}
}

/* The leaf key of scalar v at the path, under one more name when given. */
static void
lion_jsonb_emit_leaf(LionJsonbKeys *out, StringInfo buf,
					 const LionJsonbPath *path, const char *extra,
					 int extralen, const JsonbValue *v)
{
	resetStringInfo(buf);
	appendStringInfoChar(buf, LION_JK_LEAF);
	lion_jsonb_put_names(buf, path, extra, extralen);
	lion_jsonb_put_scalar(buf, v);
	lion_jsonb_put_marks(buf, path, extra != NULL);
	lion_jsonb_emit(out, buf);
}

/* The container key of an object or array at the path. */
static void
lion_jsonb_emit_container(LionJsonbKeys *out, StringInfo buf,
						  const LionJsonbPath *path, bool isarray)
{
	resetStringInfo(buf);
	appendStringInfoChar(buf, LION_JK_CONTAINER);
	lion_jsonb_put_names(buf, path, NULL, 0);
	appendStringInfoChar(buf, isarray ? 'a' : 'o');
	lion_jsonb_put_marks(buf, path, false);
	lion_jsonb_emit(out, buf);
}

static void
lion_jsonb_emit_simple(LionJsonbKeys *out, StringInfo buf, char tag,
					   const char *s, int len)
{
	resetStringInfo(buf);
	appendStringInfoChar(buf, tag);
	appendBinaryStringInfo(buf, s, len);
	lion_jsonb_emit(out, buf);
}

static void
lion_jsonb_push_name(LionJsonbPath *path, const char *name, int len)
{
	if (path->nnames + 1 >= path->maxnames)
	{
		path->maxnames *= 2;
		path->name = (const char **)
			repalloc(path->name, sizeof(char *) * path->maxnames);
		path->namelen = (int *)
			repalloc(path->namelen, sizeof(int) * path->maxnames);
		path->marks = (int *)
			repalloc(path->marks, sizeof(int) * (path->maxnames + 1));
	}
	path->name[path->nnames] = name;
	path->namelen[path->nnames] = len;
	path->nnames++;
	path->marks[path->nnames] = 0;
}

/*
 * Walk a document (query false) or an `@>` query (query true) and emit its
 * keys.  The two differ only in which keys they emit: a document every key a
 * query could look for, a query the ones its containment needs.  *lossy is
 * set for a query that has an array element contributing two keys or more.
 */
static void
lion_jsonb_walk(Jsonb *jb, bool query, LionJsonbKeys *out, bool *lossy)
{
	JsonbIterator *it;
	JsonbIteratorToken tok;
	JsonbValue	v;
	LionJsonbPath path;
	LionJsonbFrame *frames;
	int			maxframes = 16;
	int			depth = 0;
	bool		pending = false;	/* an object key was read */
	JsonbValue	pendkey;
	StringInfoData buf;

	initStringInfo(&buf);
	path.nnames = 0;
	path.maxnames = 16;
	path.name = (const char **) palloc(sizeof(char *) * path.maxnames);
	path.namelen = (int *) palloc(sizeof(int) * path.maxnames);
	path.marks = (int *) palloc0(sizeof(int) * (path.maxnames + 1));
	frames = (LionJsonbFrame *) palloc(sizeof(LionJsonbFrame) * maxframes);
	memset(&pendkey, 0, sizeof(pendkey));

	it = JsonbIteratorInit(&jb->root);
	while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		CHECK_FOR_INTERRUPTS();

		switch (tok)
		{
			case WJB_BEGIN_ARRAY:
			case WJB_BEGIN_OBJECT:
				{
					bool		isarray = (tok == WJB_BEGIN_ARRAY);
					bool		empty = isarray ? (v.val.array.nElems == 0) :
						(v.val.object.nPairs == 0);
					LionJsonbFrame *f;

					if (depth >= maxframes)
					{
						maxframes *= 2;
						frames = (LionJsonbFrame *)
							repalloc(frames, sizeof(LionJsonbFrame) * maxframes);
					}
					f = &frames[depth];
					f->isarray = isarray;
					f->named = false;
					f->element = false;
					f->firstkey = out->nkeys;

					if (depth == 0)
					{
						/*
						 * The root: a raw scalar is an array of one element
						 * to the iterator, and gets no root key.
						 */
						if (!(isarray && v.val.array.rawScalar) &&
							(!query || isarray || empty))
							lion_jsonb_emit_simple(out, &buf, LION_JK_ROOT,
												   isarray ? "a" : "o", 1);
					}
					else
					{
						if (pending)
						{
							lion_jsonb_push_name(&path, pendkey.val.string.val,
												 pendkey.val.string.len);
							f->named = true;
							pending = false;
						}
						else
							f->element = frames[depth - 1].isarray;
						if (!query || empty)
							lion_jsonb_emit_container(out, &buf, &path, isarray);
					}
					if (isarray)
						path.marks[path.nnames]++;
					depth++;
					break;
				}

			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				{
					LionJsonbFrame *f;

					Assert(depth > 0);
					f = &frames[--depth];
					if (f->isarray)
						path.marks[path.nnames]--;
					if (f->named)
						path.nnames--;
					if (query && f->element && out->nkeys - f->firstkey >= 2)
						*lossy = true;
					break;
				}

			case WJB_KEY:
				pendkey = v;
				pending = true;
				if (!query && depth == 1)
					lion_jsonb_emit_simple(out, &buf, LION_JK_EXISTS,
										   v.val.string.val,
										   v.val.string.len);
				break;

			case WJB_VALUE:
				Assert(pending);
				lion_jsonb_emit_leaf(out, &buf, &path, pendkey.val.string.val,
									 pendkey.val.string.len, &v);
				pending = false;
				break;

			case WJB_ELEM:
				lion_jsonb_emit_leaf(out, &buf, &path, NULL, 0, &v);
				if (!query && depth == 1 && v.type == jbvString)
					lion_jsonb_emit_simple(out, &buf, LION_JK_EXISTS,
										   v.val.string.val,
										   v.val.string.len);
				break;

			default:
				elog(ERROR, "unexpected jsonb iterator token %d", (int) tok);
		}
	}

	pfree(buf.data);
}

static int
lion_jsonb_key_cmp(const void *a, const void *b)
{
	bytea	   *x = (bytea *) DatumGetPointer(*(const Datum *) a);
	bytea	   *y = (bytea *) DatumGetPointer(*(const Datum *) b);
	int			lx = VARSIZE(x) - VARHDRSZ;
	int			ly = VARSIZE(y) - VARHDRSZ;
	int			c = memcmp(VARDATA(x), VARDATA(y), Min(lx, ly));

	if (c != 0)
		return c;
	return (lx < ly) ? -1 : ((lx > ly) ? 1 : 0);
}

/* Sort a query's keys and drop the duplicates: the AND looks each up once. */
static void
lion_jsonb_unique(LionJsonbKeys *out)
{
	int			i;
	int			n = 0;

	if (out->nkeys < 2)
		return;
	qsort(out->keys, out->nkeys, sizeof(Datum), lion_jsonb_key_cmp);
	for (i = 0; i < out->nkeys; i++)
	{
		if (n > 0 && lion_jsonb_key_cmp(&out->keys[n - 1], &out->keys[i]) == 0)
			continue;
		out->keys[n++] = out->keys[i];
	}
	out->nkeys = n;
}

/* ---------------------------------------------------------------------
 * jsonpath (DESIGN.md §43)
 * --------------------------------------------------------------------- */

/* One object key name of a jsonpath's path, and the names before it. */
typedef struct LionJpName
{
	const char *name;
	int			len;
	struct LionJpName *up;
} LionJpName;

/*
 * Where the items a jsonpath accessor chain yields are in the document.
 *
 *	indoc	false: they are not the document's (a variable, a literal, an
 *			arithmetic result), and say nothing about it.
 *	names	the object key names from the root, the last one first.  Array
 *			accessors add none: array levels are not part of a key's names.
 *	cut		an accessor the names cannot follow (`.*`, `.**`, an item
 *			method) came after them: the items lie at or below them, and
 *			once there is an item at all, something of the document is.
 */
typedef struct LionJpPath
{
	bool		indoc;
	bool		cut;
	int			nnames;
	LionJpName *last;
} LionJpPath;

typedef struct LionJpCxt
{
	LionJsonbKeys out;			/* the prefixes, unique */
	StringInfoData buf;
	int			hashed;			/* the 'H' prefix's key number, or -1 */
} LionJpCxt;

static LionKeyNode *lion_jp_bool(LionJpCxt *cx, LionJpPath path,
								 JsonPathItem *jsp, bool not);

/* The key number of prefix buf, added when it is new. */
static int
lion_jp_key(LionJpCxt *cx, const StringInfoData *buf)
{
	bytea	   *key;
	int			i;

	for (i = 0; i < cx->out.nkeys; i++)
	{
		bytea	   *k = (bytea *) DatumGetPointer(cx->out.keys[i]);

		if ((int) (VARSIZE(k) - VARHDRSZ) == buf->len &&
			memcmp(VARDATA(k), buf->data, buf->len) == 0)
			return i;
	}

	key = (bytea *) palloc(VARHDRSZ + buf->len);
	SET_VARSIZE(key, VARHDRSZ + buf->len);
	memcpy(VARDATA(key), buf->data, buf->len);
	if (cx->out.nkeys >= cx->out.maxkeys)
	{
		cx->out.maxkeys = cx->out.maxkeys * 2 + 16;
		cx->out.keys = cx->out.keys == NULL ?
			(Datum *) palloc(sizeof(Datum) * cx->out.maxkeys) :
			(Datum *) repalloc(cx->out.keys, sizeof(Datum) * cx->out.maxkeys);
	}
	cx->out.keys[cx->out.nkeys] = PointerGetDatum(key);
	return cx->out.nkeys++;
}

/* A tag byte and the path's names, with no ENDPATH: the start of a prefix. */
static void
lion_jp_put_names(StringInfo buf, char tag, const LionJpPath *path)
{
	LionJpName **names;
	LionJpName *n;
	int			i = path->nnames;

	resetStringInfo(buf);
	appendStringInfoChar(buf, tag);
	names = (LionJpName **) palloc(sizeof(LionJpName *) * Max(i, 1));
	for (n = path->last; n != NULL; n = n->up)
		names[--i] = n;
	for (i = 0; i < path->nnames; i++)
	{
		appendStringInfoChar(buf, LION_JK_STEP);
		lion_jsonb_put_bytes(buf, names[i]->name, names[i]->len);
	}
	pfree(names);
}

/*
 * The prefix in cx->buf as a leaf; or, when a key starting with it can be
 * long enough to be stored hashed, that leaf OR every hashed key: a hash
 * keeps no prefix of what it hashed.  maxtail is the most bytes such a key
 * can have past the prefix, or -1 for no limit.
 */
static LionKeyNode *
lion_jp_prefix(LionJpCxt *cx, int maxtail)
{
	LionKeyNode **args;
	StringInfoData h;

	if (maxtail >= 0 && VARHDRSZ + cx->buf.len + maxtail <= LION_MAX_KEY_SIZE)
		return lion_keynode_leaf(lion_jp_key(cx, &cx->buf));

	if (cx->hashed < 0)
	{
		initStringInfo(&h);
		appendStringInfoChar(&h, LION_JK_HASHED);
		cx->hashed = lion_jp_key(cx, &h);
		pfree(h.data);
	}
	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * 2);
	args[0] = lion_keynode_leaf(lion_jp_key(cx, &cx->buf));
	args[1] = lion_keynode_leaf(cx->hashed);
	return lion_keynode_op(LION_KN_OR, args, 2);
}

/*
 * Something of the document at or below the path's names: a leaf or a
 * container key under them.  NULL (no constraint) when there are no names,
 * which every document has something under.
 */
static LionKeyNode *
lion_jp_exists(LionJpCxt *cx, const LionJpPath *path)
{
	LionKeyNode **args;

	if (!path->indoc || path->nnames == 0)
		return NULL;
	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * 2);
	lion_jp_put_names(&cx->buf, LION_JK_LEAF, path);
	args[0] = lion_jp_prefix(cx, -1);
	lion_jp_put_names(&cx->buf, LION_JK_CONTAINER, path);
	args[1] = lion_jp_prefix(cx, -1);
	return lion_keynode_op(LION_KN_OR, args, 2);
}

/*
 * A scalar equal to v at the path: its leaf keys, which hold the names, the
 * value, and then one array-level count per name and one more - at most
 * five bytes each.  A cut path only says something is under its names.
 */
static LionKeyNode *
lion_jp_equals(LionJpCxt *cx, const LionJpPath *path, const JsonbValue *v)
{
	if (!path->indoc)
		return NULL;
	if (path->cut)
		return lion_jp_exists(cx, path);
	lion_jp_put_names(&cx->buf, LION_JK_LEAF, path);
	appendStringInfoChar(&cx->buf, LION_JK_ENDPATH);
	lion_jsonb_put_scalar(&cx->buf, v);
	return lion_jp_prefix(cx, 5 * (path->nnames + 1));
}

/* The AND of a list of nodes, or NULL for none. */
static LionKeyNode *
lion_jp_and(List *nodes)
{
	LionKeyNode **args;
	ListCell   *lc;
	int			n = 0;

	if (nodes == NIL)
		return NULL;
	args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * list_length(nodes));
	foreach(lc, nodes)
		args[n++] = (LionKeyNode *) lfirst(lc);
	return lion_keynode_op(LION_KN_AND, args, n);
}

/*
 * Follow an accessor chain from jsp, starting at path (the filter's current
 * item `@`, or nowhere), into *end; add to *nodes what its filters require.
 * Those hold whenever the chain yields an item: a filter passes only items
 * it is true for.
 */
static void
lion_jp_chain(LionJpCxt *cx, LionJpPath path, JsonPathItem *jsp,
			  LionJpPath *end, List **nodes)
{
	JsonPathItem next;
	bool		first = true;

	check_stack_depth();

	for (;;)
	{
		switch (jsp->type)
		{
			case jpiRoot:
				path.indoc = first;
				path.cut = false;
				path.nnames = 0;
				path.last = NULL;
				break;

			case jpiCurrent:
				if (!first)
					path.indoc = false;
				break;

			case jpiKey:
				if (!path.cut)
				{
					LionJpName *n = (LionJpName *) palloc(sizeof(LionJpName));

					n->name = jspGetString(jsp, &n->len);
					n->up = path.last;
					path.last = n;
					path.nnames++;
				}
				break;

			case jpiAnyArray:
			case jpiIndexArray:
				/* array levels are not in the names; lax treats a non-array
				 * as an array of one */
				break;

			case jpiFilter:
				{
					JsonPathItem arg;
					LionKeyNode *f;

					jspGetArg(jsp, &arg);
					f = lion_jp_bool(cx, path, &arg, false);
					if (f != NULL)
						*nodes = lappend(*nodes, f);
					break;
				}

			default:

				/*
				 * Anything else at the start - a variable, a literal, an
				 * arithmetic or boolean expression - yields items that are
				 * not the document's.  Further along - `.*`, `.**`, an item
				 * method - it takes an item and yields items the names
				 * cannot follow.
				 */
				if (first)
					path.indoc = false;
				else
					path.cut = true;
				break;
		}

		if (!path.indoc)
			break;
		first = false;
		if (!jspGetNext(jsp, &next))
			break;
		jsp = &next;
	}

	*end = path;
}

/* What `exists (chain)` requires: its filters, and something at its end. */
static LionKeyNode *
lion_jp_exists_chain(LionJpCxt *cx, LionJpPath path, JsonPathItem *jsp)
{
	List	   *nodes = NIL;
	LionJpPath end;
	LionKeyNode *e;

	lion_jp_chain(cx, path, jsp, &end, &nodes);
	e = lion_jp_exists(cx, &end);
	if (e != NULL)
		nodes = lappend(nodes, e);
	return lion_jp_and(nodes);
}

/* A jsonpath scalar literal as a JsonbValue. */
static void
lion_jp_scalar(JsonPathItem *jsp, JsonbValue *v)
{
	switch (jsp->type)
	{
		case jpiNull:
			v->type = jbvNull;
			break;
		case jpiBool:
			v->type = jbvBool;
			v->val.boolean = jspGetBool(jsp);
			break;
		case jpiNumeric:
			v->type = jbvNumeric;
			v->val.numeric = jspGetNumeric(jsp);
			break;
		case jpiString:
			v->type = jbvString;
			v->val.string.val = jspGetString(jsp, &v->val.string.len);
			break;
		default:
			elog(ERROR, "unexpected jsonpath scalar type %d", (int) jsp->type);
	}
}

/*
 * What a comparison or a string predicate being true requires.  It is true
 * only for some item of each operand, so each operand that is a chain yields
 * one: its filters hold, and something is at its end.  `chain == scalar`,
 * which jsonpath answers with byte-equal strings and equal numbers - what the
 * leaf keys store - is the leaf keys of that value.
 */
static LionKeyNode *
lion_jp_compare(LionJpCxt *cx, LionJpPath path, JsonPathItem *jsp)
{
	JsonPathItem ops[2];
	int			nops = 2;
	List	   *nodes = NIL;
	int			i;

	if (jsp->type == jpiLikeRegex)
	{
		jspInitByBuffer(&ops[0], jsp->base, jsp->content.like_regex.expr);
		nops = 1;
	}
	else
	{
		jspGetLeftArg(jsp, &ops[0]);
		jspGetRightArg(jsp, &ops[1]);
	}

	for (i = 0; i < nops; i++)
	{
		JsonPathItem *other = nops == 2 ? &ops[1 - i] : NULL;
		LionJpPath end;
		LionKeyNode *n;

		if (jspIsScalar(ops[i].type))
			continue;
		lion_jp_chain(cx, path, &ops[i], &end, &nodes);
		if (jsp->type == jpiEqual && other != NULL &&
			jspIsScalar(other->type))
		{
			JsonbValue	v;

			lion_jp_scalar(other, &v);
			n = lion_jp_equals(cx, &end, &v);
		}
		else
			n = lion_jp_exists(cx, &end);
		if (n != NULL)
			nodes = lappend(nodes, n);
	}
	return lion_jp_and(nodes);
}

/*
 * A tree that selects at least the documents for which the jsonpath
 * predicate jsp is true (not false: is false), or NULL when no tree narrower
 * than every document does.  path is what `@` stands for.
 *
 * What is taken apart, as GIN's jsonb_ops does, and a little more:
 *
 *	- `a && b`, `a || b`, `!a`: the AND or OR of the parts, with NOT pushed
 *	  down to the leaves; an AND keeps the parts that narrow, an OR needs
 *	  both.  Which of the two an operator is under a NOT is decided by the
 *	  NOT too: `!(a && b)` is an OR.
 *	- `exists (chain)`, and a comparison, `starts with` or `like_regex`
 *	  (lion_jp_compare()): never under a NOT, where `$.a != 1` being false
 *	  includes $.a being absent.
 *	- `is unknown` and anything else: no tree.
 */
static LionKeyNode *
lion_jp_bool(LionJpCxt *cx, LionJpPath path, JsonPathItem *jsp, bool not)
{
	check_stack_depth();

	switch (jsp->type)
	{
		case jpiAnd:
		case jpiOr:
			{
				JsonPathItem arg;
				LionKeyNode *l;
				LionKeyNode *r;
				LionKeyNode **args;
				bool		isand = (jsp->type == jpiAnd) != not;

				jspGetLeftArg(jsp, &arg);
				l = lion_jp_bool(cx, path, &arg, not);
				jspGetRightArg(jsp, &arg);
				r = lion_jp_bool(cx, path, &arg, not);
				if (l == NULL || r == NULL)
					return isand ? (l != NULL ? l : r) : NULL;
				args = (LionKeyNode **) palloc(sizeof(LionKeyNode *) * 2);
				args[0] = l;
				args[1] = r;
				return lion_keynode_op(isand ? LION_KN_AND : LION_KN_OR, args, 2);
			}

		case jpiNot:
			{
				JsonPathItem arg;

				jspGetArg(jsp, &arg);
				return lion_jp_bool(cx, path, &arg, !not);
			}

		case jpiExists:
			{
				JsonPathItem arg;

				if (not)
					return NULL;
				jspGetArg(jsp, &arg);
				return lion_jp_exists_chain(cx, path, &arg);
			}

		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
		case jpiStartsWith:
		case jpiLikeRegex:
			if (not)
				return NULL;
			return lion_jp_compare(cx, path, jsp);

		default:
			return NULL;
	}
}

/*
 * The jsonpath query: keys (all prefixes) and the tree over them, or false
 * when nothing narrower than every document can be said.  `doc @? path` is
 * `doc @@ exists (path)`.
 */
static bool
lion_jp_query(JsonPath *jp, StrategyNumber strategy, LionJsonbKeys *out,
			  LionKeyNode **tree)
{
	LionJpCxt	cx;
	LionJpPath path;
	JsonPathItem root;

	memset(&cx, 0, sizeof(cx));
	cx.hashed = -1;
	initStringInfo(&cx.buf);
	memset(&path, 0, sizeof(path));

	jspInit(&root, jp);
	if (strategy == LION_STRAT_JSONPATH_EXISTS)
		*tree = lion_jp_exists_chain(&cx, path, &root);
	else
		*tree = lion_jp_bool(&cx, path, &root, false);

	pfree(cx.buf.data);
	*out = cx.out;
	return *tree != NULL;
}

PG_FUNCTION_INFO_V1(lion_jsonb_extract_value);

/*
 * lion_jsonb_extract_value(jsonb, internal, internal) returns internal:
 * support function 2, GIN's extractValue signature.  Duplicates are left in;
 * lion_extract_value() drops them.
 */
Datum
lion_jsonb_extract_value(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	bool	  **nullFlags = (bool **) PG_GETARG_POINTER(2);
	LionJsonbKeys out;
	bool		lossy = false;

	memset(&out, 0, sizeof(out));
	lion_jsonb_walk(jb, false, &out, &lossy);

	*nentries = out.nkeys;
	if (nullFlags != NULL)
		*nullFlags = NULL;
	PG_RETURN_POINTER(out.keys);
}

PG_FUNCTION_INFO_V1(lion_jsonb_extract_query);

/*
 * lion_jsonb_extract_query(jsonb, internal, int2, internal, internal,
 * internal, internal) returns internal: support function 3, GIN's
 * extractQuery signature, with lion's strategy numbers (lion_gin_strategy()
 * passes them through) and one search mode of lion's own,
 * LION_SEARCH_MODE_LOSSY, for keys that select a superset.
 */
Datum
lion_jsonb_extract_query(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	LionJsonbKeys out;
	bool		lossy = false;
	StringInfoData buf;

	memset(&out, 0, sizeof(out));
	*searchMode = GIN_SEARCH_MODE_DEFAULT;

	switch (strategy)
	{
		case LION_STRAT_JSONB_CONTAINS:
			lion_jsonb_walk(PG_GETARG_JSONB_P(0), true, &out, &lossy);
			break;

		case LION_STRAT_JSONB_EXISTS:
			{
				text	   *name = PG_GETARG_TEXT_PP(0);

				initStringInfo(&buf);
				lion_jsonb_emit_simple(&out, &buf, LION_JK_EXISTS,
									   VARDATA_ANY(name),
									   VARSIZE_ANY_EXHDR(name));
				pfree(buf.data);
				break;
			}

		case LION_STRAT_JSONB_EXISTS_ANY:
		case LION_STRAT_JSONB_EXISTS_ALL:
			{
				ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
				Datum	   *elems;
				bool	   *nulls;
				int			nelems;
				int			i;

				deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &nelems);
				initStringInfo(&buf);
				for (i = 0; i < nelems; i++)
				{
					text	   *name;

					if (nulls[i])
						continue;
					name = DatumGetTextPP(elems[i]);
					lion_jsonb_emit_simple(&out, &buf, LION_JK_EXISTS,
										   VARDATA_ANY(name),
										   VARSIZE_ANY_EXHDR(name));
				}
				pfree(buf.data);

				/* `?& '{}'` holds for every document; `?| '{}'` for none */
				if (out.nkeys == 0 && strategy == LION_STRAT_JSONB_EXISTS_ALL)
					*searchMode = GIN_SEARCH_MODE_ALL;
				break;
			}

		case LION_STRAT_JSONPATH_EXISTS:
		case LION_STRAT_JSONPATH_MATCH:
			{
				bool	  **pmatch = (bool **) PG_GETARG_POINTER(3);
				Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);
				LionKeyNode *tree;
				int			i;

				/*
				 * The tree goes to lion_extract_query_superset() in
				 * extra_data[0]; every key is a prefix (pmatch), and every
				 * answer is rechecked.
				 */
				if (!lion_jp_query(PG_GETARG_JSONPATH_P(0), strategy, &out,
								   &tree))
				{
					*nentries = 0;
					*searchMode = GIN_SEARCH_MODE_ALL;
					PG_RETURN_POINTER(NULL);
				}
				*pmatch = (bool *) palloc(sizeof(bool) * out.nkeys);
				for (i = 0; i < out.nkeys; i++)
					(*pmatch)[i] = true;
				*extra_data = (Pointer *) palloc0(sizeof(Pointer) * out.nkeys);
				(*extra_data)[0] = (Pointer) tree;
				*searchMode = LION_SEARCH_MODE_LOSSY;
				*nentries = out.nkeys;
				PG_RETURN_POINTER(out.keys);
			}

		default:
			elog(ERROR, "lion_jsonb_extract_query: unknown strategy %d",
				 (int) strategy);
	}

	lion_jsonb_unique(&out);
	if ((lossy || out.hashed) && *searchMode == GIN_SEARCH_MODE_DEFAULT)
		*searchMode = LION_SEARCH_MODE_LOSSY;

	*nentries = out.nkeys;
	PG_RETURN_POINTER(out.keys);
}
