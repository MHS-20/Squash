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

/* ---- naive ----------------------------------------------------------- */

static lz_match naive_find(const uint8_t *in, size_t n, size_t pos, const lz_params *p)
{
    lz_match best = {0, 0};
    uint32_t max = max_len_at(n, pos, p);
    for (uint32_t d = 1; d <= p->max_dist && d <= pos; d++) {
        uint32_t len = common_prefix(in + pos - d, in + pos, max);
        if (len > best.len)
            best = (lz_match){len, d};
    }
    if (best.len < p->min_match)
        best.len = 0;
    return best;
}

int lz_parse_naive(const uint8_t *in, size_t n, const lz_params *p, lz_seqs *out)
{
    size_t pos = 0, lit_start = 0;
    while (pos < n) {
        lz_match m = naive_find(in, n, pos, p);
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

int lz_matcher_init(lz_matcher *m, const uint8_t *in, size_t n, const lz_params *p)
{
    m->in = in;
    m->n = n;
    m->p = p;
    m->head = calloc((size_t)1 << HASH_BITS, sizeof(uint32_t));
    m->prev = calloc(p->window, sizeof(uint32_t));
    if (m->head && m->prev)
        return SQ_OK;
    lz_matcher_free(m);
    return SQ_ERR_MEM;
}

void lz_matcher_free(lz_matcher *m)
{
    free(m->head);
    free(m->prev);
    m->head = m->prev = NULL;
}

static uint32_t hash_at(const lz_matcher *m, size_t pos)
{
    const uint8_t *s = m->in + pos;
    uint32_t v = s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16;
    if (m->p->min_match >= 4)
        v |= (uint32_t)s[3] << 24;
    /* Multiplicative (Fibonacci) hashing: the top bits of the product
     * depend on every input bit. */
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

void lz_matcher_insert(lz_matcher *m, size_t pos)
{
    if (pos + m->p->min_match > m->n)
        return;  /* too close to the end to ever start a match */
    uint32_t h = hash_at(m, pos);
    m->prev[pos & (m->p->window - 1)] = m->head[h];
    m->head[h] = (uint32_t)pos + 1;
}

lz_match lz_matcher_find(const lz_matcher *m, size_t pos)
{
    const lz_params *p = m->p;
    lz_match best = {0, 0};
    uint32_t max = max_len_at(m->n, pos, p);
    if (max < p->min_match)
        return best;
    uint32_t cand = m->head[hash_at(m, pos)];
    for (uint32_t tries = p->max_chain; cand && tries--; ) {
        size_t cpos = cand - 1;
        size_t dist = pos - cpos;
        if (dist > p->max_dist)
            break;  /* chains run newest to oldest, so the rest are farther */
        const uint8_t *a = m->in + cpos, *b = m->in + pos;
        /* The byte that would extend the best match must agree, otherwise
         * this candidate cannot beat it: a cheap early reject. */
        if (a[best.len] == b[best.len]) {
            uint32_t len = common_prefix(a, b, max);
            if (len > best.len) {
                best = (lz_match){len, (uint32_t)dist};
                if (len >= p->nice_len || len == max)
                    break;
            }
        }
        cand = m->prev[cpos & (p->window - 1)];
    }
    if (best.len < p->min_match)
        best.len = 0;
    return best;
}

int lz_parse(const uint8_t *in, size_t n, const lz_params *p, lz_seqs *out)
{
    lz_matcher c;
    int err = lz_matcher_init(&c, in, n, p);
    size_t pos = 0, lit_start = 0;
    lz_match pending = {0, 0};  /* lazy: the match found at pos - 1 */

    while (!err && pos < n) {
        lz_match m = lz_matcher_find(&c, pos);
        lz_matcher_insert(&c, pos);
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
            lz_matcher_insert(&c, i);
        pos = start + m.len;
        lit_start = pos;
    }
    if (!err && pending.len) {
        err = emit(out, (uint32_t)(pos - 1 - lit_start), pending.len, pending.dist);
        lit_start = pos - 1 + pending.len;
    }
    if (!err)
        err = emit(out, (uint32_t)(n - lit_start), 0, 0);
    if (c.head)
        lz_matcher_free(&c);
    return err;
}
