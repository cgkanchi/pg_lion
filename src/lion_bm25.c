/*-------------------------------------------------------------------------
 *
 * lion_bm25.c
 *		BM25 ranking from a lion index that stores positions (DESIGN.md §17,
 *		"Ranking").
 *
 *			lion_bm25(index regclass, query tsquery, k int,
 *					  k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75)
 *			RETURNS TABLE (ctid tid, score float8)
 *
 * returns the k rows of the index's table with the highest Okapi BM25 score
 * for the query's lexemes, best first, each with the TID of its version the
 * caller's snapshot sees - so `JOIN t ON t.ctid = s.ctid` reads exactly those
 * rows.  Everything the score needs comes from the index:
 *
 *	tf(t, d)	the number of positions row d has under lexeme t, a stripped
 *				lexeme counting once: the npos of d's member in t's position
 *				tree;
 *	df(t)		the rows with a member under t;
 *	|d|			the row's length: the one "position" the row has under the
 *				empty key, which the extraction files for every row with a
 *				lexeme (lion_extract_internal());
 *	N, avgdl	the members of the empty key and the mean of their lengths,
 *				which a backend computes once and keeps until the empty
 *				key's row count has moved by more than 1/64 or the index has
 *				been rebuilt (lion_bm25_stats()).
 *
 *		score(d) = sum over t of idf(t) * tf * (k1 + 1) /
 *								(tf + k1 * (1 - b + b * |d| / avgdl))
 *		idf(t)   = ln(1 + (N - df + 0.5) / (df + 0.5))
 *
 * which is Lucene's BM25 (the "+ 1" keeps idf positive).  The query's lexemes
 * are its operands, each counted once, except those under a NOT; its
 * operators, phrase distances and weights do not change the score, so a
 * caller who wants only the rows that match the query as a whole adds the
 * `@@` itself.  A prefix lexeme is refused.
 *
 * The heap is read only for the rows returned: each candidate, best first,
 * is looked up under the caller's snapshot until k are visible.  Like df and
 * N, the statistics count rows VACUUM has not removed yet, as a search
 * engine's do until a merge drops its deleted documents.
 *
 * Every member of every query lexeme is read once per call, and the empty
 * key's member of each row that has one: the work grows with the rows that
 * have the lexemes, not with k.  The candidates stream out of a merge of the
 * lexemes' members in TID order, each scored as it comes, and only the best
 * k + 16 are sorted; the rest are sorted only when that many are not enough
 * because some are invisible to the snapshot.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "utils/acl.h"
#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "lion.h"
#include "lion_compat.h"
#include "lion_count.h"
#include "lion_positions.h"
#include "lion_tid.h"

PG_FUNCTION_INFO_V1(lion_bm25);

/* One lexeme's members: (code, tf) in TID order, and where the merge is. */
typedef struct LionBm25Term
{
	uint64	   *codes;
	uint16	   *tfs;
	int64		n;
	int64		next;
	double		idf;
} LionBm25Term;

/* A candidate row. */
typedef struct LionBm25Cand
{
	uint64		code;
	double		score;
} LionBm25Cand;

/* N and the sum of the lengths, per index, for this backend. */
typedef struct LionBm25Stats
{
	Oid			indexoid;		/* hash key */
	RelFileNumber relnumber;	/* the index's storage they were read from */
	uint64		ntids;			/* the empty key's ntids then */
	int64		nrows;			/* its members */
	double		sumlen;			/* the sum of their lengths */
} LionBm25Stats;

static HTAB *lion_bm25_stats_cache = NULL;

/*
 * The lexemes of query that count: every operand not under a NOT, once.
 * A prefix operand is refused, since it names no one key.
 */
static void
lion_bm25_terms_walk(TSQuery query, int i, bool negated, List **out)
{
	QueryItem  *item = GETQUERY(query) + i;

	check_stack_depth();

	if (item->type == QI_VAL)
	{
		QueryOperand *op = &item->qoperand;
		text	   *lex;
		ListCell   *lc;

		if (op->prefix)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lion_bm25 cannot rank a prefix lexeme")));
		if (negated)
			return;
		lex = cstring_to_text_with_len(GETOPERAND(query) + op->distance,
									   op->length);
		foreach(lc, *out)
		{
			text	   *t = (text *) lfirst(lc);

			if (VARSIZE_ANY_EXHDR(t) == VARSIZE_ANY_EXHDR(lex) &&
				memcmp(VARDATA_ANY(t), VARDATA_ANY(lex),
					   VARSIZE_ANY_EXHDR(lex)) == 0)
				return;
		}
		*out = lappend(*out, lex);
		return;
	}

	/* an operator: its right operand at i + 1, its left at i + left */
	if (item->qoperator.oper == OP_NOT)
	{
		lion_bm25_terms_walk(query, i + 1, !negated, out);
		return;
	}
	lion_bm25_terms_walk(query, i + 1, negated, out);
	lion_bm25_terms_walk(query, i + item->qoperator.left, negated, out);
}

static LionPosCursor *
lion_bm25_open(Relation index, LionState *col, Datum key, bool countsonly)
{
	LionPosCursor *cur = lion_poscursor_open(index, col, key, countsonly);

	if (cur == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has an entry without positions",
						RelationGetRelationName(index)),
				 errhint("REINDEX the index.")));
	return cur;
}

/* Read every member of a lexeme: its codes and term frequencies. */
static void
lion_bm25_read_term(Relation index, LionState *col, Datum key,
					LionBm25Term *term)
{
	LionPosCursor *cur = lion_bm25_open(index, col, key, true);
	const LionPosMember *m;
	int64		cap = 1024;
	uint64		code = 0;

	term->n = 0;
	term->next = 0;
	term->codes = (uint64 *) palloc(sizeof(uint64) * cap);
	term->tfs = (uint16 *) palloc(sizeof(uint16) * cap);
	while ((m = lion_poscursor_next(index, cur, code)) != NULL)
	{
		if (term->n == cap)
		{
			cap *= 2;
			term->codes = (uint64 *)
				repalloc_huge(term->codes, sizeof(uint64) * cap);
			term->tfs = (uint16 *)
				repalloc_huge(term->tfs, sizeof(uint16) * cap);
		}
		term->codes[term->n] = m->code;
		term->tfs[term->n] = Max(m->npos, 1);	/* a stripped lexeme: once */
		term->n++;
		code = m->code + 1;
		if ((term->n & 0xfff) == 0)
			CHECK_FOR_INTERRUPTS();
	}
	lion_poscursor_close(cur);
}

/* The length a member of the empty key holds: its one "position". */
static inline double
lion_bm25_length(const LionPosMember *m)
{
	return (m->npos > 0) ? (double) m->pos[0] : 0;
}

/*
 * N and the sum of the lengths: walked from the empty key, or this backend's
 * last walk while the key's ntids is within 1/64 of what it was then and the
 * index has not been rebuilt.  False when no row has a lexeme.
 */
static bool
lion_bm25_stats(Relation index, LionState *col, Datum lenkey,
				int64 *nrows, double *sumlen)
{
	LionBm25Stats *st;
	uint64		ntids = 0;
	bool		found;
	Buffer		buf = InvalidBuffer;
	OffsetNumber off;

	if (lion_find_entry(index, col, BUFFER_LOCK_SHARE, lenkey,
						lion_hash_key(col, lenkey), &buf, &off))
	{
		Page		page = BufferGetPage(buf);

		ntids = lion_page_entry_fetch(index, page, BufferGetBlockNumber(buf),
									  off)->ntids;
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	if (ntids == 0)
		return false;

	if (lion_bm25_stats_cache == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(LionBm25Stats);
		lion_bm25_stats_cache = hash_create("lion bm25 statistics", 16, &ctl,
											HASH_ELEM | HASH_BLOBS);
	}
	st = (LionBm25Stats *) hash_search(lion_bm25_stats_cache,
									   &RelationGetRelid(index), HASH_ENTER,
									   &found);
	if (!found || st->relnumber != index->rd_locator.relNumber ||
		(ntids > st->ntids ? ntids - st->ntids : st->ntids - ntids) >
		st->ntids / 64)
	{
		LionPosCursor *cur = lion_bm25_open(index, col, lenkey, false);
		const LionPosMember *m;
		uint64		code = 0;
		int64		n = 0;
		double		sum = 0;

		while ((m = lion_poscursor_next(index, cur, code)) != NULL)
		{
			sum += lion_bm25_length(m);
			n++;
			code = m->code + 1;
			if ((n & 0xfff) == 0)
				CHECK_FOR_INTERRUPTS();
		}
		lion_poscursor_close(cur);
		st->relnumber = index->rd_locator.relNumber;
		st->ntids = ntids;
		st->nrows = n;
		st->sumlen = sum;
	}
	*nrows = st->nrows;
	*sumlen = st->sumlen;
	return st->nrows > 0;
}

/* Best first; equal scores in TID order, so the answer is deterministic. */
static inline bool
lion_bm25_better(const LionBm25Cand *x, const LionBm25Cand *y)
{
	if (x->score != y->score)
		return x->score > y->score;
	return x->code < y->code;
}

static int
lion_bm25_cand_cmp(const void *a, const void *b)
{
	const LionBm25Cand *x = (const LionBm25Cand *) a;
	const LionBm25Cand *y = (const LionBm25Cand *) b;

	if (lion_bm25_better(x, y))
		return -1;
	if (lion_bm25_better(y, x))
		return 1;
	return 0;
}

/*
 * A min-heap of the best L candidates seen, worst at the root: heap[] holds
 * them, n of at most L.
 */
static void
lion_bm25_heap_sift(LionBm25Cand *heap, int n, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			w = i;

		if (l < n && lion_bm25_better(&heap[w], &heap[l]))
			w = l;
		if (r < n && lion_bm25_better(&heap[w], &heap[r]))
			w = r;
		if (w == i)
			return;
		{
			LionBm25Cand tmp = heap[i];

			heap[i] = heap[w];
			heap[w] = tmp;
		}
		i = w;
	}
}

static void
lion_bm25_heap_add(LionBm25Cand *heap, int *n, int cap, const LionBm25Cand *c)
{
	if (*n < cap)
	{
		int			i = (*n)++;

		heap[i] = *c;
		while (i > 0)
		{
			int			parent = (i - 1) / 2;
			LionBm25Cand tmp;

			if (!lion_bm25_better(&heap[parent], &heap[i]))
				break;
			tmp = heap[i];
			heap[i] = heap[parent];
			heap[parent] = tmp;
			i = parent;
		}
	}
	else if (lion_bm25_better(c, &heap[0]))
	{
		heap[0] = *c;
		lion_bm25_heap_sift(heap, *n, 0);
	}
}

/* The key column of index that stores positions, 0-based. */
static int
lion_bm25_column(Relation index, LionIndexState *ix)
{
	int			found = -1;
	int			i;

	for (i = 0; i < ix->ncolumns; i++)
	{
		if (!ix->cols[i].positions || ix->cols[i].typid != TEXTOID)
			continue;
		if (found >= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("lion index \"%s\" stores positions for more than one column",
							RelationGetRelationName(index)),
					 errhint("lion_bm25 needs an index with one tsvector column.")));
		found = i;
	}
	if (found < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("lion index \"%s\" does not store positions",
						RelationGetRelationName(index)),
				 errhint("Build it WITH (store_positions = true).")));
	return found;
}

/*
 * Emit the best of cands[from, to), already sorted, that the snapshot sees,
 * until *emitted reaches k.
 */
static void
lion_bm25_emit(ReturnSetInfo *rsinfo, Relation heap, Snapshot snapshot,
			   const LionBm25Cand *cands, int64 from, int64 to, int32 k,
			   int64 *emitted)
{
	int64		i;

	for (i = from; i < to && *emitted < k; i++)
	{
		ItemPointerData tid;
		Datum		values[2];
		bool		nulls[2] = {false, false};

		/* the TID of the version the snapshot sees, a HOT chain followed */
		lion_code_to_tid(cands[i].code, &tid);
		if (!lion_table_fetch_tid(heap, &tid, snapshot, NULL))
			continue;
		values[0] = ItemPointerGetDatum(&tid);
		values[1] = Float8GetDatum(cands[i].score);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		(*emitted)++;
		CHECK_FOR_INTERRUPTS();
	}
}

Datum
lion_bm25(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	int32		k = PG_GETARG_INT32(2);
	double		k1 = PG_GETARG_FLOAT8(3);
	double		b = PG_GETARG_FLOAT8(4);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			heapoid;
	Relation	heap;
	Relation	index;
	LionIndexState *ix;
	LionState  *col;
	Datum		lenkey = PointerGetDatum(cstring_to_text_with_len("", 0));
	List	   *lexemes = NIL;
	ListCell   *lc;
	LionBm25Term *terms;
	int			nterms;
	int64		nrows;
	double		sumlen;
	double		avgdl;
	int64		ncodes = 0;
	LionBm25Cand *cands;
	int64		ncands = 0;
	LionBm25Cand *best;
	int			nbest = 0;
	int			bestcap;
	LionPosCursor *lencur;
	Snapshot	snapshot;
	int64		emitted = 0;
	int			t;

	if (k < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("k must not be negative")));
	if (!(k1 >= 0) || !(b >= 0 && b <= 1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("k1 must be at least 0 and b between 0 and 1")));

	InitMaterializedSRF(fcinfo, 0);

	/* The relations, the privilege checked before any lock. */
	if (get_rel_relkind(indexoid) != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", get_rel_name(indexoid))));
	heapoid = IndexGetRelation(indexoid, false);
	if (pg_class_aclcheck(heapoid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV,
					   get_relkind_objtype(get_rel_relkind(heapoid)),
					   get_rel_name(heapoid));
	heap = table_open(heapoid, AccessShareLock);
	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam != lion_get_am_oid())
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("index \"%s\" is not a lion index",
						RelationGetRelationName(index))));
	lion_check_table_am(heap);

	ix = lion_get_index_state(index);
	col = &ix->cols[lion_bm25_column(index, ix)];

	if (query->size > 0)
		lion_bm25_terms_walk(query, 0, false, &lexemes);
	nterms = list_length(lexemes);
	if (nterms == 0 || k == 0 ||
		!lion_bm25_stats(index, col, lenkey, &nrows, &sumlen))
		goto done;
	avgdl = sumlen / (double) nrows;
	if (avgdl <= 0)
		avgdl = 1;

	/* every query lexeme's members */
	terms = (LionBm25Term *) palloc0(sizeof(LionBm25Term) * nterms);
	t = 0;
	foreach(lc, lexemes)
	{
		LionBm25Term *term = &terms[t++];
		double		df;

		lion_bm25_read_term(index, col, PointerGetDatum(lfirst(lc)), term);
		df = (double) Min(term->n, nrows);
		term->idf = log(1.0 + ((double) nrows - df + 0.5) / (df + 0.5));
		ncodes += term->n;
	}
	if (ncodes == 0)
		goto done;

	/*
	 * Merge them in TID order: each candidate gets its length from the
	 * empty key, whose cursor only moves forward, and its score at once.
	 */
	cands = (LionBm25Cand *)
		palloc_extended(sizeof(LionBm25Cand) * ncodes, MCXT_ALLOC_HUGE);
	bestcap = (int) Min((int64) k + 16, ncodes);
	best = (LionBm25Cand *) palloc(sizeof(LionBm25Cand) * bestcap);
	lencur = lion_bm25_open(index, col, lenkey, false);
	for (;;)
	{
		uint64		code = PG_UINT64_MAX;
		const LionPosMember *m;
		double		norm;
		double		score = 0;

		for (t = 0; t < nterms; t++)
			if (terms[t].next < terms[t].n &&
				terms[t].codes[terms[t].next] < code)
				code = terms[t].codes[terms[t].next];
		if (code == PG_UINT64_MAX)
			break;

		m = lion_poscursor_next(index, lencur, code);
		norm = k1 * (1.0 - b + b *
					 ((m != NULL && m->code == code) ?
					  lion_bm25_length(m) : avgdl) / avgdl);
		for (t = 0; t < nterms; t++)
		{
			LionBm25Term *term = &terms[t];

			if (term->next < term->n && term->codes[term->next] == code)
			{
				double		tf = (double) term->tfs[term->next++];

				score += term->idf * tf * (k1 + 1.0) / (tf + norm);
			}
		}
		cands[ncands].code = code;
		cands[ncands].score = score;
		lion_bm25_heap_add(best, &nbest, bestcap, &cands[ncands]);
		ncands++;
		if ((ncands & 0xfff) == 0)
			CHECK_FOR_INTERRUPTS();
	}
	lion_poscursor_close(lencur);

	/* the best k + 16, best first; all of them only if those fall short */
	snapshot = GetActiveSnapshot();
	qsort(best, nbest, sizeof(LionBm25Cand), lion_bm25_cand_cmp);
	lion_bm25_emit(rsinfo, heap, snapshot, best, 0, nbest, k, &emitted);
	if (emitted < k && ncands > nbest)
	{
		qsort(cands, ncands, sizeof(LionBm25Cand), lion_bm25_cand_cmp);
		lion_bm25_emit(rsinfo, heap, snapshot, cands, nbest, ncands, k,
					   &emitted);
	}

done:
	index_close(index, AccessShareLock);
	table_close(heap, AccessShareLock);
	return (Datum) 0;
}
