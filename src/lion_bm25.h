/*-------------------------------------------------------------------------
 *
 * lion_bm25.h
 *		BM25 ranking from a lion index that stores positions (lion_bm25.c),
 *		for lion_bm25(), lion_bm25_score() and the LionBm25 scan
 *		(lion_bm25_scan.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef LION_BM25_H
#define LION_BM25_H

#include "tsearch/ts_type.h"
#include "utils/rel.h"

#include "lion.h"

/* A candidate row: its TID's code and its score. */
typedef struct LionBm25Cand
{
	uint64		code;
	double		score;
} LionBm25Cand;

/*
 * A query made ready to rank against one index: its lexemes, each one's
 * idf, and the order in which a score adds them up - ascending by the most
 * a lexeme can add, which both the walk and lion_bm25_score_row() follow
 * from the end, so a row gets the same score bit for bit from either.
 */
typedef struct LionBm25Query
{
	Relation	index;			/* open while the walk runs; NULL after */
	LionState  *col;
	int			nterms;
	text	  **lexemes;
	double	   *idfs;
	int		   *order;			/* lexeme indexes, ascending idf */
	double		k1;
	double		b;
	double		avgdl;
	uint64		ncodes;			/* the lexemes' rows, added up */
} LionBm25Query;

extern int	lion_bm25_column(Relation index, LionIndexState *ix);
extern List *lion_bm25_lexemes(TSQuery query, bool *prefix);
extern void lion_bm25_check_params(double k1, double b);
extern bool lion_bm25_prepare(Relation index, TSQuery query, double k1,
							  double b, LionBm25Query *q);
extern int64 lion_bm25_topk(const LionBm25Query *q, int64 L,
							const LionBm25Cand *after, LionBm25Cand *best);
extern int64 lion_bm25_next_L(int64 L, int64 first, const LionBm25Query *q,
							  int64 seen);
extern void lion_bm25_sort(LionBm25Cand *cands, int64 n);
extern double lion_bm25_score_row(const LionBm25Query *q, TSVector doc);

/* lion_bm25_scan.c */
extern PGDLLIMPORT bool lion_enable_bm25_scan;
extern void lion_bm25_scan_init(void);

#endif							/* LION_BM25_H */
