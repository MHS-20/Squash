/* LZ77 parsing: turn input into sequences of (literal run, back-reference).
 *
 * Every LZ77-family format here (DEFLATE, LZ4, Snappy) differs only in how
 * it serialises sequences, so one parser serves them all, configured with the
 * format's limits. */
#ifndef SQ_LZ77_H
#define SQ_LZ77_H

#include <stddef.h>
#include <stdint.h>

/* `lit_len` literal bytes, then `match_len` bytes copied from `dist` back.
 * The final sequence of a parse has match_len == 0 (trailing literals). */
typedef struct {
    uint32_t lit_len;
    uint32_t match_len;
    uint32_t dist;
} lz_seq;

typedef struct {
    lz_seq *seq;
    size_t n, cap;
} lz_seqs;

typedef struct {
    uint32_t window;      /* history kept by the hash chains; power of two */
    uint32_t max_dist;    /* format's largest distance (<= window) */
    uint32_t min_match;   /* 3 or 4: shorter matches cost more than literals */
    uint32_t max_match;
    uint32_t max_chain;   /* candidates tried per position: speed vs ratio */
    uint32_t nice_len;    /* stop searching once a match this long is found */
    uint32_t lazy;        /* 0: greedy. Else defer a match shorter than this
                             by one byte if the next position matches longer */
    uint32_t end_margin;  /* matches must stop this many bytes before the end */
    uint32_t start_margin;/* and start at least this many before it */
} lz_params;

/* The hash-chain match finder on its own, for encoders that drive their
 * own parse (LZMA): find the longest match at a position, then insert it.
 * head[hash] is the newest position whose first bytes hash there and
 * prev[pos % window] links each position to the previous one. */
typedef struct {
    const uint8_t *in;
    size_t n;
    size_t limit;           /* matches end by here; callers coding in blocks lower it */
    const lz_params *p;
    uint32_t *head, *prev;  /* positions + 1; 0 = none */
} lz_matcher;

typedef struct {
    uint32_t len, dist;     /* len 0: no match of at least min_match */
} lz_match;

int lz_matcher_init(lz_matcher *m, const uint8_t *in, size_t n, const lz_params *p);
void lz_matcher_free(lz_matcher *m);
lz_match lz_matcher_find(const lz_matcher *m, size_t pos);
void lz_matcher_insert(lz_matcher *m, size_t pos);

void lz_seqs_init(lz_seqs *s);
void lz_seqs_free(lz_seqs *s);

/* Hash-chain parser (greedy or lazy, per `p->lazy`). */
int lz_parse(const uint8_t *in, size_t n, const lz_params *p, lz_seqs *out);

/* Brute force: tries every distance at every position. Exponentially
 * slower; it exists as the obviously-correct baseline for tests and as the
 * starting point the hash chains improve on. */
int lz_parse_naive(const uint8_t *in, size_t n, const lz_params *p, lz_seqs *out);

#endif
