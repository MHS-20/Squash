#include "lz77.h"

#include <stdlib.h>
#include <string.h>

#include "squash.h"

#define HASH_BITS 15

void lz_seqs_init(lz_seqs *s) { s->seq = NULL; s->n = s->cap = 0; }

void lz_seqs_free(lz_seqs *s) { free(s->seq); lz_seqs_init(s); }

static int emit(lz_seqs *s, uint32_t lit_len, uint32_t match_len, uint32_t dist)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? 2 * s->cap : 1024;
        lz_seq *p = realloc(s->seq, cap * sizeof *p);
        if (!p)
            return SQ_ERR_MEM;
        s->seq = p;
        s->cap = cap;
    }
    s->seq[s->n++] = (lz_seq){lit_len, match_len, dist};
    return SQ_OK;
}

/* Longest length at which a[] and b[] agree, up to max. */
static uint32_t common_prefix(const uint8_t *a, const uint8_t *b, uint32_t max)
{
    uint32_t len = 0;
    while (len < max && a[len] == b[len])
        len++;
    return len;
}

/* Bytes a match starting at pos may cover, given the format's limits. */
static uint32_t max_len_at(size_t n, size_t pos, const lz_params *p)
{
    if (n < p->end_margin || pos + p->start_margin >= n)
        return 0;
    size_t avail = n - p->end_margin - pos;
    return avail < p->max_match ? (uint32_t)avail : p->max_match;
}

typedef struct {
    uint32_t len, dist;
} match;

/* ---- naive ----------------------------------------------------------- */

static match naive_find(const uint8_t *in, size_t n, size_t pos, const lz_params *p)
{
    match best = {0, 0};
    uint32_t max = max_len_at(n, pos, p);
    for (uint32_t d = 1; d <= p->max_dist && d <= pos; d++) {
        uint32_t len = common_prefix(in + pos - d, in + pos, max);
        if (len > best.len)
            best = (match){len, d};
    }
    if (best.len < p->min_match)
        best.len = 0;
    return best;
}

int lz_parse_naive(const uint8_t *in, size_t n, const lz_params *p, lz_seqs *out)
{
    size_t pos = 0, lit_start = 0;
    while (pos < n) {
        match m = naive_find(in, n, pos, p);
        if (m.len == 0) {
            pos++;
            continue;
        }
        int err = emit(out, (uint32_t)(pos - lit_start), m.len, m.dist);
        if (err)
            return err;
        pos += m.len;
        lit_start = pos;
    }
    return emit(out, (uint32_t)(n - lit_start), 0, 0);
}

/* ---- hash chains ----------------------------------------------------- */

/* head[h] is the most recent position whose first min_match bytes hash to
 * h; prev[pos % window] links each position to the previous one with the
 * same hash. Positions are stored +1 so that 0 means "none". */
typedef struct {
    const uint8_t *in;
    size_t n;
    const lz_params *p;
    uint32_t *head, *prev;
} chains;

static uint32_t hash_at(const chains *c, size_t pos)
{
    const uint8_t *s = c->in + pos;
    uint32_t v = s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16;
    if (c->p->min_match >= 4)
        v |= (uint32_t)s[3] << 24;
    /* Multiplicative (Fibonacci) hashing: the top bits of the product
     * depend on every input bit. */
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

static void insert(chains *c, size_t pos)
{
    if (pos + c->p->min_match > c->n)
        return;  /* too close to the end to ever start a match */
    uint32_t h = hash_at(c, pos);
    c->prev[pos & (c->p->window - 1)] = c->head[h];
    c->head[h] = (uint32_t)pos + 1;
}

static match chain_find(const chains *c, size_t pos)
{
    const lz_params *p = c->p;
    match best = {0, 0};
    uint32_t max = max_len_at(c->n, pos, p);
    if (max < p->min_match)
        return best;
    uint32_t cand = c->head[hash_at(c, pos)];
    for (uint32_t tries = p->max_chain; cand && tries--; ) {
        size_t cpos = cand - 1;
        size_t dist = pos - cpos;
        if (dist > p->max_dist)
            break;  /* chains run newest to oldest, so the rest are farther */
        const uint8_t *a = c->in + cpos, *b = c->in + pos;
        /* The byte that would extend the best match must agree, otherwise
         * this candidate cannot beat it: a cheap early reject. */
        if (a[best.len] == b[best.len]) {
            uint32_t len = common_prefix(a, b, max);
            if (len > best.len) {
                best = (match){len, (uint32_t)dist};
                if (len >= p->nice_len || len == max)
                    break;
            }
        }
        cand = c->prev[cpos & (p->window - 1)];
    }
    if (best.len < p->min_match)
        best.len = 0;
    return best;
}

int lz_parse(const uint8_t *in, size_t n, const lz_params *p, lz_seqs *out)
{
    chains c = {in, n, p, calloc(1u << HASH_BITS, sizeof(uint32_t)),
                calloc(p->window, sizeof(uint32_t))};
    int err = (c.head && c.prev) ? SQ_OK : SQ_ERR_MEM;
    size_t pos = 0, lit_start = 0;
    match pending = {0, 0};  /* lazy: the match found at pos - 1 */

    while (!err && pos < n) {
        match m = chain_find(&c, pos);
        insert(&c, pos);
        size_t start;  /* where the match being committed begins */
        if (pending.len) {
            if (m.len > pending.len) {
                /* The later match wins: pos-1 becomes a literal and the
                 * new match is itself deferred for one more look. */
                pending = m;
                pos++;
                continue;
            }
            m = pending;
            pending.len = 0;
            start = pos - 1;
        } else {
            if (m.len == 0) {
                pos++;
                continue;
            }
            if (m.len < p->lazy && pos + 1 < n) {
                pending = m;
                pos++;
                continue;
            }
            start = pos;
        }
        err = emit(out, (uint32_t)(start - lit_start), m.len, m.dist);
        /* Index every covered position so later matches can refer into
         * this one; `pos` itself is already inserted. */
        for (size_t i = pos + 1; i < start + m.len; i++)
            insert(&c, i);
        pos = start + m.len;
        lit_start = pos;
    }
    if (!err && pending.len) {
        err = emit(out, (uint32_t)(pos - 1 - lit_start), pending.len, pending.dist);
        lit_start = pos - 1 + pending.len;
    }
    if (!err)
        err = emit(out, (uint32_t)(n - lit_start), 0, 0);
    free(c.head);
    free(c.prev);
    return err;
}
