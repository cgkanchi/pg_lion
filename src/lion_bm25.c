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
 * snapshot does not see k of those k + 16, the walk is made again for the
 * next four times as many, ranked below the last of them.
 *
 * Like an ordinary scan of the index, the function refuses a table with
 * row-level security and an index the snapshot cannot use, and takes the
 * index's predicate lock before reading (lion_reader_open()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "lion.h"
#include "lion_bm25.h"
#include "lion_compat.h"
#include "lion_count.h"
#include "lion_positions.h"
#include "lion_tid.h"

PG_FUNCTION_INFO_V1(lion_bm25);
PG_FUNCTION_INFO_V1(lion_bm25_score);

/*
 * One query lexeme in a walk: a cursor over its members in TID order, the member it
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
 * A prefix operand names no one key: it is refused, or with prefix given,
 * reported there.
 */
static void
lion_bm25_terms_walk(TSQuery query, int i, bool negated, bool *prefix,
					 List **out)
{
	QueryItem  *item = GETQUERY(query) + i;

	check_stack_depth();

	if (item->type == QI_VAL)
	{
		QueryOperand *op = &item->qoperand;
		text	   *lex;
		ListCell   *lc;

		if (op->prefix && prefix != NULL)
		{
			*prefix = true;
			return;
		}
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
		lion_bm25_terms_walk(query, i + 1, !negated, prefix, out);
		return;
	}
	lion_bm25_terms_walk(query, i + 1, negated, prefix, out);
	lion_bm25_terms_walk(query, i + item->qoperator.left, negated, prefix,
						 out);
}

List *
lion_bm25_lexemes(TSQuery query, bool *prefix)
{
	List	   *out = NIL;

	if (prefix != NULL)
		*prefix = false;
	if (query->size > 0)
		lion_bm25_terms_walk(query, 0, false, prefix, &out);
	return out;
}

void
lion_bm25_check_params(double k1, double b)
{
	if (!(k1 >= 0) || !(b >= 0 && b <= 1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("k1 must be at least 0 and b between 0 and 1")));
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
int
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
 * Emit the best of cands[0, n), already sorted, that the snapshot sees,
 * until *emitted reaches k.
 */
static void
lion_bm25_emit(ReturnSetInfo *rsinfo, Relation heap, Snapshot snapshot,
			   const LionBm25Cand *cands, int64 n, int32 k, int64 *emitted)
{
	int64		i;

	for (i = 0; i < n && *emitted < k; i++)
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

/* What tf occurrences of a lexeme of idf idf add to a row whose norm is norm. */
static inline double
lion_bm25_tf_score(double idf, double tf, double k1, double norm)
{
	return idf * tf * (k1 + 1.0) / (tf + norm);
}

/* k1 * (1 - b + b * |d| / avgdl) */
static inline double
lion_bm25_norm(const LionBm25Query *q, double dl)
{
	return q->k1 * (1.0 - q->b + q->b * dl / q->avgdl);
}

/* The order of a query's lexemes: by idf, then as the query has them. */
static const double *lion_bm25_sort_idfs;

static int
lion_bm25_order_cmp(const void *a, const void *b)
{
	int			x = *(const int *) a;
	int			y = *(const int *) b;
	double		ix = lion_bm25_sort_idfs[x];
	double		iy = lion_bm25_sort_idfs[y];

	if (ix != iy)
		return (ix > iy) - (ix < iy);
	return (x > y) - (x < y);
}

/*
 * Make query ready to rank against index, which must be a lion index that
 * stores positions for one tsvector column.  False when no row can score:
 * no lexeme counts, no row has one, or none has any of the query's.
 */
bool
lion_bm25_prepare(Relation index, TSQuery query, double k1, double b,
				  LionBm25Query *q)
{
	LionIndexState *ix = lion_get_index_state(index);
	Datum		lenkey = PointerGetDatum(cstring_to_text_with_len("", 0));
	List	   *lexemes;
	ListCell   *lc;
	int64		nrows;
	int			t;

	memset(q, 0, sizeof(LionBm25Query));
	q->index = index;
	q->col = &ix->cols[lion_bm25_column(index, ix)];
	q->k1 = k1;
	q->b = b;

	lexemes = lion_bm25_lexemes(query, NULL);
	q->nterms = list_length(lexemes);
	if (q->nterms == 0 ||
		!lion_bm25_stats(index, q->col, lenkey, &nrows, &q->avgdl))
		return false;

	/* each lexeme's idf, from the rows its entry says it has */
	q->lexemes = (text **) palloc(sizeof(text *) * q->nterms);
	q->idfs = (double *) palloc(sizeof(double) * q->nterms);
	q->order = (int *) palloc(sizeof(int) * q->nterms);
	t = 0;
	foreach(lc, lexemes)
	{
		uint64		df = lion_bm25_ntids(index, q->col,
										 PointerGetDatum(lfirst(lc)));

		q->ncodes += df;
		df = Min(df, (uint64) nrows);
		q->lexemes[t] = (text *) lfirst(lc);
		q->idfs[t] = log(1.0 + ((double) nrows - (double) df + 0.5) /
						 ((double) df + 0.5));
		q->order[t] = t;
		t++;
	}
	lion_bm25_sort_idfs = q->idfs;
	qsort(q->order, q->nterms, sizeof(int), lion_bm25_order_cmp);
	return q->ncodes > 0;
}

/*
 * How many rows the next walk asks for, after one that asked for L (0: none
 * yet) and the walks' seen rows: first, then four times as many - but no
 * more than one past the rows the lexemes had when q was prepared.  Rows
 * inserted since are in the index too, and the snapshot does not see them;
 * so a walk that fills its L is never taken to be the last, however many it
 * has given in all, and only one that comes up short is.  The one extra row
 * is what lets the walk that reaches the end come up short without a walk
 * more.
 */
int64
lion_bm25_next_L(int64 L, int64 first, const LionBm25Query *q, int64 seen)
{
	L = (L == 0) ? first : L * 4;
	if ((int64) q->ncodes > seen)
		L = Min(L, (int64) q->ncodes - seen + 1);
	return Max(L, 1);
}

/* Best first, equal scores in TID order. */
void
lion_bm25_sort(LionBm25Cand *cands, int64 n)
{
	qsort(cands, n, sizeof(LionBm25Cand), lion_bm25_cand_cmp);
}

/*
 * The score of one row from its tsvector, as the walk would give it: the
 * same tf (a stripped lexeme once), the same length (every occurrence of
 * every lexeme but an empty one, capped at 65535) and the same order of
 * adding up.
 */
double
lion_bm25_score_row(const LionBm25Query *q, TSVector doc)
{
	WordEntry  *we = ARRPTR(doc);
	char	   *str = STRPTR(doc);
	uint32		dl = 0;
	double		norm;
	double		score = 0;
	int			i;
	int			t;

	for (i = 0; i < doc->size; i++)
		if (we[i].len > 0)
			dl += we[i].haspos ? Max((uint32) POSDATALEN(doc, &we[i]), 1) : 1;
	norm = lion_bm25_norm(q, (double) Min(dl, (uint32) PG_UINT16_MAX));

	for (t = q->nterms - 1; t >= 0; t--)
	{
		text	   *lex = q->lexemes[q->order[t]];
		int			lo = 0;
		int			hi = doc->size;

		while (lo < hi)
		{
			int			mid = lo + (hi - lo) / 2;
			int32		c = tsCompareString(VARDATA_ANY(lex),
											VARSIZE_ANY_EXHDR(lex),
											str + we[mid].pos, we[mid].len,
											false);

			if (c == 0)
			{
				double		tf = we[mid].haspos ?
					(double) Max(POSDATALEN(doc, &we[mid]), 1) : 1.0;

				score += lion_bm25_tf_score(q->idfs[q->order[t]], tf, q->k1,
											norm);
				break;
			}
			if (c < 0)
				hi = mid;
			else
				lo = mid + 1;
		}
	}
	return score;
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
 *
 * With after given, only the rows ranked below it count: the walk goes on
 * from where an earlier one with the same LionBm25Query stopped.  A row's
 * score depends only on its members and on q, which neither changes, so the
 * rows ranked below after are the ones the earlier walk had not reached -
 * plus rows inserted since, which the caller's snapshot does not see.  An
 * offset into a bigger walk would not do: a row inserted meanwhile above it
 * moves every row after it down one place, and one would be returned twice
 * and another never.
 */
int64
lion_bm25_topk(const LionBm25Query *q, int64 L, const LionBm25Cand *after,
			   LionBm25Cand *best)
{
	Relation	index = q->index;
	int			nterms = q->nterms;
	LionBm25Term *terms = (LionBm25Term *) palloc0(sizeof(LionBm25Term) * nterms);
	double	   *below;			/* below[i]: the maxima of terms[0, i) */
	Datum		lenkey = PointerGetDatum(cstring_to_text_with_len("", 0));
	LionPosCursor *lencur;
	int64		nbest = 0;
	int			nlow = 0;
	int64		nscored = 0;
	int			t;

	below = (double *) palloc(sizeof(double) * (nterms + 1));
	below[0] = 0;
	for (t = 0; t < nterms; t++)
	{
		LionBm25Term *term = &terms[t];
		int			l = q->order[t];

		term->cur = lion_bm25_open(index, q->col,
								   PointerGetDatum(q->lexemes[l]), true);
		term->m = lion_poscursor_next(index, term->cur, 0);
		term->idf = q->idfs[l];
		term->ub = q->idfs[l] * (q->k1 + 1.0) * (1.0 + 1e-9);
		below[t + 1] = below[t] + term->ub;
	}

	lencur = lion_bm25_open(index, q->col, lenkey, false);
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
		norm = lion_bm25_norm(q, (m != NULL && m->code == code) ?
							  lion_bm25_length(m) : q->avgdl);

		/* the essential terms, then the rest from the one that can add most */
		for (t = nterms - 1; t >= nlow; t--)
		{
			LionBm25Term *term = &terms[t];

			if (term->m != NULL && term->m->code == code)
			{
				score += lion_bm25_tf_score(term->idf,
											(double) Max(term->m->npos, 1),
											q->k1, norm);
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
				score += lion_bm25_tf_score(term->idf,
											(double) Max(term->m->npos, 1),
											q->k1, norm);
		}
		c.code = code;
		c.score = score;
		if (t < 0 && (nbest < L || score > best[0].score) &&
			(after == NULL || lion_bm25_better(after, &c)))
		{
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
	Relation	heap;
	Relation	index;
	LionBm25Query q;
	LionBm25Cand after;
	bool		have_after = false;
	int64		L;
	int64		seen = 0;
	Snapshot	snapshot;
	int64		emitted = 0;

	if (k < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("k must not be negative")));
	lion_bm25_check_params(k1, b);

	InitMaterializedSRF(fcinfo, 0);

	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "lion_bm25 requires an active snapshot");
	index = lion_reader_open(indexoid, LION_READ_ROWS, snapshot, "rank",
							 &heap);
	if (!lion_bm25_prepare(index, query, k1, b, &q) || k == 0)
		goto out;

	/*
	 * The best k + 16, best first, until k of them are visible: when the
	 * snapshot does not see enough of them, the next 4 times as many, ranked
	 * below the last of those already tried, until a walk comes up short
	 * (lion_bm25_next_L()).
	 */
	L = lion_bm25_next_L(0, (int64) k + 16, &q, 0);
	for (;;)
	{
		LionBm25Cand *best = (LionBm25Cand *)
			palloc_extended(sizeof(LionBm25Cand) * L, MCXT_ALLOC_HUGE);
		int64		nbest;

		nbest = lion_bm25_topk(&q, L, have_after ? &after : NULL, best);
		lion_bm25_sort(best, nbest);
		lion_bm25_emit(rsinfo, heap, snapshot, best, nbest, k, &emitted);
		seen += nbest;
		if (nbest > 0)
		{
			after = best[nbest - 1];
			have_after = true;
		}
		pfree(best);
		if (emitted >= k || nbest < L)
			break;
		L = lion_bm25_next_L(L, (int64) k + 16, &q, seen);
	}

out:
	index_close(index, AccessShareLock);
	table_close(heap, AccessShareLock);
	return (Datum) 0;
}

/* What lion_bm25_score() keeps across the rows of one call site. */
typedef struct LionBm25ScoreCache
{
	MemoryContext cxt;			/* what follows points into; reset on change */
	Oid			indexoid;
	double		k1;
	double		b;
	TSQuery		query;			/* a copy */
	bool		valid;			/* any row can score */
	LionBm25Query q;
} LionBm25ScoreCache;

/*
 * lion_bm25_score(doc tsvector, query tsquery, index regclass,
 *				   k1 float8 DEFAULT 1.2, b float8 DEFAULT 0.75) RETURNS float8
 *
 * The BM25 score lion_bm25() would give a row whose tsvector is doc, with
 * the statistics of index, read once per call site and query.  Under
 * `ORDER BY lion_bm25_score(...) DESC`, a LionBm25 scan returns the rows in
 * that order from the index (lion_bm25_scan.c); anywhere else this scores
 * one row at a time.
 */
Datum
lion_bm25_score(PG_FUNCTION_ARGS)
{
	TSVector	doc = PG_GETARG_TSVECTOR(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);
	Oid			indexoid = PG_GETARG_OID(2);
	double		k1 = PG_GETARG_FLOAT8(3);
	double		b = PG_GETARG_FLOAT8(4);
	LionBm25ScoreCache *cache = (LionBm25ScoreCache *) fcinfo->flinfo->fn_extra;

	if (cache == NULL || cache->query == NULL ||
		cache->indexoid != indexoid || cache->k1 != k1 ||
		cache->b != b || VARSIZE(cache->query) != VARSIZE(query) ||
		memcmp(cache->query, query, VARSIZE(query)) != 0)
	{
		MemoryContext old;
		Relation	index;

		lion_bm25_check_params(k1, b);

		/*
		 * The arguments may change from row to row: what the last ones
		 * prepared goes with them, rather than piling up in fn_mcxt until
		 * the statement ends.
		 */
		if (cache == NULL)
		{
			cache = (LionBm25ScoreCache *)
				MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
									   sizeof(LionBm25ScoreCache));
			cache->cxt = AllocSetContextCreate(fcinfo->flinfo->fn_mcxt,
											   "lion_bm25_score cache",
											   ALLOCSET_SMALL_SIZES);
			fcinfo->flinfo->fn_extra = cache;
		}
		else
			MemoryContextReset(cache->cxt);
		cache->query = NULL;
		old = MemoryContextSwitchTo(cache->cxt);
		index = lion_reader_open(indexoid, LION_READ_STATS, NULL, "score",
								 NULL);
		cache->valid = lion_bm25_prepare(index, query, k1, b, &cache->q);
		cache->q.index = NULL;
		cache->q.col = NULL;
		index_close(index, AccessShareLock);
		cache->indexoid = indexoid;
		cache->k1 = k1;
		cache->b = b;
		cache->query = (TSQuery) palloc(VARSIZE(query));
		memcpy(cache->query, query, VARSIZE(query));
		MemoryContextSwitchTo(old);
	}
	if (!cache->valid)
		PG_RETURN_FLOAT8(0);
	PG_RETURN_FLOAT8(lion_bm25_score_row(&cache->q, doc));
}
