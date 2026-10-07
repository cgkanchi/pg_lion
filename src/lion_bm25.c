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
 *	df(t)		the rows filed under t: its entry's ntids;
 *	|d|			the row's length: the one "position" the row has under the
 *				empty key, which the extraction files for every row with a
 *				lexeme (lion_extract_internal());
 *	N			the rows filed under the empty key;
 *	avgdl		the mean length of the empty key's members, which a backend
 *				computes once and keeps until the empty key's row count has
 *				moved by more than 1/64 or the index has been rebuilt
 *				(lion_bm25_stats()).
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
 * The candidates stream out of a merge of the lexemes' members in TID order,
 * each scored as it comes into a heap of the best k + 16, pruned by MaxScore
 * (lion_bm25_topk()): once the heap is full, the lexemes that together
 * cannot lift a row past its worst are only looked up for rows the others
 * bring, so a common lexeme next to a rarer one is mostly skipped.  When the
 * snapshot does not see k of those k + 16, the walk is made again for four
 * times as many.
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

/*
 * One query lexeme: a cursor over its members in TID order, the member it
 * stands on (NULL once past the last), its idf, and the most it can add to a
 * score.
 */
typedef struct LionBm25Term
{
	LionPosCursor *cur;
	const LionPosMember *m;
	double		idf;
	double		ub;
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

/* The rows filed under key: its entry's ntids, 0 when it has none. */
static uint64
lion_bm25_ntids(Relation index, LionState *col, Datum key)
{
	uint64		ntids = 0;
	Buffer		buf = InvalidBuffer;
	OffsetNumber off;

	if (lion_find_entry(index, col, BUFFER_LOCK_SHARE, key,
						lion_hash_key(col, key), &buf, &off))
	{
		Page		page = BufferGetPage(buf);

		ntids = lion_page_entry_fetch(index, page, BufferGetBlockNumber(buf),
									  off)->ntids;
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	return ntids;
}

/* The length a member of the empty key holds: its one "position". */
static inline double
lion_bm25_length(const LionPosMember *m)
{
	return (m->npos > 0) ? (double) m->pos[0] : 0;
}

/*
 * N, the empty key's ntids, and avgdl: the mean length of the empty key's
 * members, walked from it, or this backend's last walk while the key's ntids
 * is within 1/64 of what it was then and the index has not been rebuilt.
 * False when no row has a lexeme.
 */
static bool
lion_bm25_stats(Relation index, LionState *col, Datum lenkey,
				int64 *nrows, double *avgdl)
{
	LionBm25Stats *st;
	uint64		ntids = lion_bm25_ntids(index, col, lenkey);
	bool		found;

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
	if (st->nrows == 0)
		return false;
	*nrows = (int64) ntids;
	*avgdl = st->sumlen / (double) st->nrows;
	if (*avgdl <= 0)
		*avgdl = 1;
	return true;
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
lion_bm25_heap_sift(LionBm25Cand *heap, int64 n, int64 i)
{
	for (;;)
	{
		int64		l = 2 * i + 1;
		int64		r = l + 1;
		int64		w = i;

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
lion_bm25_heap_add(LionBm25Cand *heap, int64 *n, int64 cap, const LionBm25Cand *c)
{
	if (*n < cap)
	{
		int64		i = (*n)++;

		heap[i] = *c;
		while (i > 0)
		{
			int64		parent = (i - 1) / 2;
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


/* BM25 of one member: what tf adds to a row whose norm is norm. */
static inline double
lion_bm25_term_score(const LionBm25Term *term, const LionPosMember *m,
					 double k1, double norm)
{
	double		tf = (double) Max(m->npos, 1);	/* a stripped lexeme: once */

	return term->idf * tf * (k1 + 1.0) / (tf + norm);
}

static int
lion_bm25_term_ub_cmp(const void *a, const void *b)
{
	const LionBm25Term *x = (const LionBm25Term *) a;
	const LionBm25Term *y = (const LionBm25Term *) b;

	return (x->ub > y->ub) - (x->ub < y->ub);
}

/*
 * The best L rows into best[], unsorted; returns how many.  MaxScore (Turtle
 * and Flood): the terms in ascending order of the most they can add, the
 * first nlow of them "non-essential" while those maxima sum to no more than
 * the worst score the heap holds once it is full.  A row with none of the
 * other, essential, terms then cannot get in, so only the essential terms'
 * members are candidates, merged in TID order; each non-essential term is
 * sought only for a candidate, best first, and only while what is left of
 * them could still lift it past the heap's worst.  The heap's worst only
 * rises, so nlow only grows.  A row's score is the same sum the full merge
 * would make, in the same term order.
 *
 * A term adds less than idf * (k1 + 1) to any row, since tf / (tf + norm) is
 * below 1 (and at most 1 when k1 is 0); that, nudged up so a sum of maxima
 * rounded down cannot pass for a score, is its maximum.  Equal scores rank by
 * TID and a candidate's TID is above every TID the heap holds, so a candidate
 * gets in only with a score above the heap's worst: at a bound no higher than
 * that, a row is not scored on.
 */
static int64
lion_bm25_topk(Relation index, LionState *col, Datum lenkey, List *lexemes,
			   const double *idfs, double k1, double b, double avgdl,
			   int64 L, LionBm25Cand *best)
{
	int			nterms = list_length(lexemes);
	LionBm25Term *terms = (LionBm25Term *) palloc0(sizeof(LionBm25Term) * nterms);
	double	   *below;			/* below[i]: the maxima of terms[0, i) */
	LionPosCursor *lencur;
	int64		nbest = 0;
	int			nlow = 0;
	int64		nscored = 0;
	ListCell   *lc;
	int			t;

	t = 0;
	foreach(lc, lexemes)
	{
		LionBm25Term *term = &terms[t];

		term->cur = lion_bm25_open(index, col, PointerGetDatum(lfirst(lc)),
								   true);
		term->m = lion_poscursor_next(index, term->cur, 0);
		term->idf = idfs[t];
		term->ub = idfs[t] * (k1 + 1.0) * (1.0 + 1e-9);
		t++;
	}
	qsort(terms, nterms, sizeof(LionBm25Term), lion_bm25_term_ub_cmp);
	below = (double *) palloc(sizeof(double) * (nterms + 1));
	below[0] = 0;
	for (t = 0; t < nterms; t++)
		below[t + 1] = below[t] + terms[t].ub;

	lencur = lion_bm25_open(index, col, lenkey, false);
	for (;;)
	{
		uint64		code = PG_UINT64_MAX;
		const LionPosMember *m;
		double		norm;
		double		score = 0;
		LionBm25Cand c;

		for (t = nlow; t < nterms; t++)
			if (terms[t].m != NULL && terms[t].m->code < code)
				code = terms[t].m->code;
		if (code == PG_UINT64_MAX)
			break;

		m = lion_poscursor_next(index, lencur, code);
		norm = k1 * (1.0 - b + b *
					 ((m != NULL && m->code == code) ?
					  lion_bm25_length(m) : avgdl) / avgdl);

		/* the essential terms, then the rest from the one that can add most */
		for (t = nterms - 1; t >= nlow; t--)
		{
			LionBm25Term *term = &terms[t];

			if (term->m != NULL && term->m->code == code)
			{
				score += lion_bm25_term_score(term, term->m, k1, norm);
				term->m = lion_poscursor_next(index, term->cur, code + 1);
			}
		}
		for (t = nlow - 1; t >= 0; t--)
		{
			LionBm25Term *term = &terms[t];

			if (nbest == L && score + below[t + 1] <= best[0].score)
				break;
			if (term->m != NULL && term->m->code < code)
				term->m = lion_poscursor_next(index, term->cur, code);
			if (term->m != NULL && term->m->code == code)
				score += lion_bm25_term_score(term, term->m, k1, norm);
		}
		if (t < 0 && (nbest < L || score > best[0].score))
		{
			c.code = code;
			c.score = score;
			lion_bm25_heap_add(best, &nbest, L, &c);
			while (nbest == L && nlow < nterms &&
				   below[nlow + 1] <= best[0].score)
				nlow++;
		}
		if ((++nscored & 0xfff) == 0)
			CHECK_FOR_INTERRUPTS();
	}
	lion_poscursor_close(lencur);
	for (t = 0; t < nterms; t++)
		lion_poscursor_close(terms[t].cur);
	pfree(terms);
	pfree(below);
	return nbest;
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
	double	   *idfs;
	int			nterms;
	int64		nrows;
	double		avgdl;
	uint64		ncodes = 0;
	int64		L;
	int64		done = 0;
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
		!lion_bm25_stats(index, col, lenkey, &nrows, &avgdl))
		goto out;

	/* each lexeme's idf, from the rows its entry says it has */
	idfs = (double *) palloc(sizeof(double) * nterms);
	t = 0;
	foreach(lc, lexemes)
	{
		uint64		df = lion_bm25_ntids(index, col,
										 PointerGetDatum(lfirst(lc)));

		ncodes += df;
		df = Min(df, (uint64) nrows);
		idfs[t++] = log(1.0 + ((double) nrows - (double) df + 0.5) /
						((double) df + 0.5));
	}
	if (ncodes == 0)
		goto out;

	/*
	 * The best k + 16, best first, until k of them are visible: when the
	 * snapshot does not see enough of them, the best 4 times as many, of
	 * which the first are the ones already tried.
	 */
	snapshot = GetActiveSnapshot();
	L = Min((int64) k + 16, (int64) ncodes);
	for (;;)
	{
		LionBm25Cand *best = (LionBm25Cand *)
			palloc_extended(sizeof(LionBm25Cand) * L, MCXT_ALLOC_HUGE);
		int64		nbest;

		nbest = lion_bm25_topk(index, col, lenkey, lexemes, idfs, k1, b,
							   avgdl, L, best);
		qsort(best, nbest, sizeof(LionBm25Cand), lion_bm25_cand_cmp);
		lion_bm25_emit(rsinfo, heap, snapshot, best, done, nbest, k, &emitted);
		pfree(best);
		if (emitted >= k || nbest < L || L >= (int64) ncodes)
			break;
		done = nbest;
		L = Min(L * 4, (int64) ncodes);
	}

out:
	index_close(index, AccessShareLock);
	table_close(heap, AccessShareLock);
	return (Datum) 0;
}
