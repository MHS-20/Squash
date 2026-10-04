/* zstd encoder: a greedy/lazy LZ77 parse with repeat-offset preference,
 * cut into blocks of at most 128 KiB. Each block codes its literals with
 * Huffman (or raw/RLE) and its sequences with per-type FSE tables, choosing
 * predefined, RLE or freshly built tables by estimated size. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "checksum.h"
#include "lz77.h"
#include "zstd.h"

/* Window log, chain length and lazy threshold per level (1..19). */
static const struct {
    uint8_t window_log;
    uint16_t chain;
    uint16_t lazy;
} levels[20] = {
    {0, 0, 0},
    {19, 2, 0}, {19, 4, 0}, {20, 8, 16}, {20, 16, 16}, {21, 16, 32}, {21, 32, 32},
    {22, 32, 64}, {22, 48, 64}, {22, 64, 128}, {23, 64, 128}, {23, 96, 128},
    {23, 128, 256}, {23, 128, 256}, {23, 192, 256}, {23, 256, 256}, {23, 256, 512},
    {23, 384, 512}, {23, 512, 512}, {23, 768, 1024},
};

#define ZSTD_MIN_MATCH 3
#define ZSTD_MAX_MATCH 131074

typedef struct {
    uint32_t lit_len, offset_value, match_len;
} zseq;

typedef struct {
    const uint8_t *in;
    size_t n;
    lz_params params;
    lz_matcher finder;
    uint32_t lazy;
    uint32_t rep[3];       /* as the decoder will see them */
    zseq *seq;
    size_t nseq;
    uint8_t *lit;
    size_t nlit;
} zstd_cctx;

static unsigned highbit(uint32_t v) { return 31 - (unsigned)__builtin_clz(v); }

/* ---- parsing --------------------------------------------------------- */

/* Picks the cheapest way to name `offset` given the repeat history. */
static uint32_t offset_value_for(const uint32_t rep[3], uint32_t offset, uint32_t lit_len)
{
    if (lit_len > 0) {
        for (uint32_t i = 0; i < 3; i++)
            if (offset == rep[i])
                return i + 1;
    } else {
        if (offset == rep[1])
            return 1;
        if (offset == rep[2])
            return 2;
        if (offset == rep[0] - 1)
            return 3;
    }
    return offset + 3;
}

static uint32_t match_len_at(const zstd_cctx *c, size_t pos, size_t dist, size_t end)
{
    if (dist == 0 || dist > pos)
        return 0;
    const uint8_t *a = c->in + pos, *b = a - dist;
    size_t max = end - pos < ZSTD_MAX_MATCH ? end - pos : ZSTD_MAX_MATCH;
    uint32_t len = 0;
    while (len < max && a[len] == b[len])
        len++;
    return len;
}

static void add_seq(zstd_cctx *c, size_t lit_start, size_t pos, uint32_t offset, uint32_t len)
{
    uint32_t ll = (uint32_t)(pos - lit_start);
    memcpy(c->lit + c->nlit, c->in + lit_start, ll);
    c->nlit += ll;
    uint32_t ov = offset_value_for(c->rep, offset, ll);
    zstd_resolve_offset(c->rep, ov, ll);
    c->seq[c->nseq++] = (zseq){ll, ov, len};
}

/* Parses [start, end): positions before start are already indexed. */
static void parse_block(zstd_cctx *c, size_t start, size_t end)
{
    c->nseq = c->nlit = 0;
    c->finder.limit = end;
    size_t pos = start, lit_start = start;
    while (pos + ZSTD_MIN_MATCH <= end) {
        lz_match main = lz_matcher_find(&c->finder, pos);
        lz_matcher_insert(&c->finder, pos);
        /* A repeat offset costs only a few bits, so it wins unless the
         * new match is clearly longer. */
        uint32_t rep_len = 0, rep_off = 0;
        for (int r = 0; r < 3; r++) {
            uint32_t len = match_len_at(c, pos, c->rep[r], end);
            if (len > rep_len)
                rep_len = len, rep_off = c->rep[r];
        }
        uint32_t len, off;
        if (rep_len >= ZSTD_MIN_MATCH && rep_len + 1 >= main.len) {
            len = rep_len, off = rep_off;
        } else if (main.len >= ZSTD_MIN_MATCH) {
            len = main.len, off = main.dist;
            if (len < c->lazy && pos + 1 + ZSTD_MIN_MATCH <= end &&
                lz_matcher_find(&c->finder, pos + 1).len > len) {
                pos++;  /* a longer match starts next: emit a literal */
                continue;
            }
        } else {
            pos++;
            continue;
        }
        add_seq(c, lit_start, pos, off, len);
        for (size_t i = pos + 1; i < pos + len; i++)
            lz_matcher_insert(&c->finder, i);
        pos += len;
        lit_start = pos;
    }
    for (size_t i = pos; i < end; i++)
        lz_matcher_insert(&c->finder, i);
    memcpy(c->lit + c->nlit, c->in + lit_start, end - lit_start);
    c->nlit += end - lit_start;
}

/* ---- literals section ------------------------------------------------ */

static int raw_or_rle_header(sq_buf *out, unsigned type, size_t size)
{
    if (size < 32)
        return sq_buf_push(out, (uint8_t)(type | size << 3));
    if (size < 4096)
        return sq_buf_put_le(out, type | 1u << 2 | size << 4, 2);
    return sq_buf_put_le(out, type | 3u << 2 | size << 4, 3);
}

static int write_literals(sq_buf *out, const uint8_t *lit, size_t n)
{
    uint32_t count[256] = {0};
    for (size_t i = 0; i < n; i++)
        count[lit[i]]++;
    if (n > 0 && count[lit[0]] == n) {
        int err = raw_or_rle_header(out, ZSTD_LIT_RLE, n);
        return err ? err : sq_buf_push(out, lit[0]);
    }
    huf0_ctable t;
    sq_buf body;
    sq_buf_init(&body);
    int four = n >= 256;  /* four streams decode faster; compulsory past 1 KiB */
    int err = n < 64 ? SQ_ERR_ARG : huf0_build(&t, count);
    if (!err)
        err = huf0_write_table(&body, &t);
    if (!err)
        err = huf0_encode_literals(&body, &t, lit, n, four);
    /* Huffman only pays if it beats the raw bytes by the header cost. */
    if (!err && body.len + 2 < n && (body.len < 1024 || four)) {
        unsigned sf = !four ? 0 : (n < 1024 && body.len < 1024) ? 1
                                : (n < 16384 && body.len < 16384) ? 2 : 3;
        static const uint8_t bits[4] = {10, 10, 14, 18};
        uint64_t h = ZSTD_LIT_COMPRESSED | sf << 2 | (uint64_t)n << 4 | (uint64_t)body.len << (4 + bits[sf]);
        err = sq_buf_put_le(out, h, sf < 2 ? 3 : sf == 2 ? 4 : 5);
        if (!err)
            err = sq_buf_append(out, body.data, body.len);
        sq_buf_free(&body);
        return err;
    }
    sq_buf_free(&body);
    err = raw_or_rle_header(out, ZSTD_LIT_RAW, n);
    return err ? err : sq_buf_append(out, lit, n);
}

/* ---- sequences section ----------------------------------------------- */

static unsigned ll_code(uint32_t ll)
{
    unsigned c = ZSTD_LL_CODES - 1;
    while (zstd_ll_base[c] > ll)
        c--;
    return c;
}

static unsigned ml_code(uint32_t ml)
{
    unsigned c = ZSTD_ML_CODES - 1;
    while (zstd_ml_base[c] > ml)
        c--;
    return c;
}

/* Estimated bits to code `count` with `norm` (a state of a symbol with
 * k of 2^log states costs about log - log2 k bits). */
static double table_cost(const uint32_t *count, unsigned nsyms, const fse_norm *norm,
                         unsigned norm_syms, unsigned log)
{
    double bits = 0;
    for (unsigned s = 0; s < nsyms; s++) {
        if (!count[s])
            continue;
        if (s >= norm_syms || norm[s] == 0)
            return INFINITY;  /* the table cannot code this symbol */
        bits += count[s] * (log - (norm[s] < 0 ? 0 : log2(norm[s])));
    }
    return bits;
}

typedef struct {
    unsigned mode;
    fse_ctable t;
    sq_buf desc;  /* the table description to emit, if any */
} seq_coder;

static int choose_table(seq_coder *sc, int type, const uint32_t *count, unsigned nsyms,
                        size_t nseq)
{
    sq_buf_init(&sc->desc);
    unsigned used = 0, last = 0;
    for (unsigned s = 0; s < nsyms; s++)
        if (count[s])
            used++, last = s;
    if (used == 1) {
        /* RLE: one symbol, so states carry no information (log 0). */
        sc->mode = ZSTD_MODE_RLE;
        sc->t.log = 0;
        return sq_buf_push(&sc->desc, (uint8_t)last);
    }
    double pre = table_cost(count, nsyms, zstd_default_norm[type], zstd_default_nsyms[type],
                            zstd_default_log[type]);
    unsigned log = highbit((uint32_t)nseq) + 1;
    log = log < FSE_MIN_LOG ? FSE_MIN_LOG : log > zstd_max_log[type] ? zstd_max_log[type] : log;
    while ((1u << log) < used)
        log++;  /* every used symbol needs a state */
    fse_norm norm[ZSTD_ML_CODES];
    int err = fse_normalize(count, last + 1, nseq, log, 1, norm);
    if (err)
        return err;
    if ((err = fse_write_ncount(&sc->desc, norm, last + 1, log)))
        return err;
    double fresh = table_cost(count, last + 1, norm, last + 1, log) + 8.0 * sc->desc.len;
    if (pre <= fresh) {
        sc->mode = ZSTD_MODE_PREDEFINED;
        sc->desc.len = 0;
        return fse_build_ctable(&sc->t, zstd_default_norm[type], zstd_default_nsyms[type],
                                zstd_default_log[type]);
    }
    sc->mode = ZSTD_MODE_FSE;
    return fse_build_ctable(&sc->t, norm, last + 1, log);
}

/* With a zero-bit table (RLE mode) states carry no information. */
static void encode_sym(fse_cstate *s, bitwriter_lsb *bw, unsigned sym)
{
    if (s->t->log)
        fse_encode(s, bw, sym);
}

static int write_sequences(sq_buf *out, const zseq *seq, size_t nseq)
{
    int err;
    if (nseq < 128) {
        err = sq_buf_push(out, (uint8_t)nseq);
    } else if (nseq < 0x7F00) {
        err = sq_buf_put_be(out, nseq | 0x8000, 2);
    } else {
        err = sq_buf_push(out, 255);
        if (!err)
            err = sq_buf_put_le(out, nseq - 0x7F00, 2);
    }
    if (err || nseq == 0)
        return err;

    uint8_t *code[3];
    for (int k = 0; k < 3; k++)
        if (!(code[k] = malloc(nseq)))
            return SQ_ERR_MEM;
    uint32_t count[3][ZSTD_ML_CODES] = {{0}};
    for (size_t i = 0; i < nseq; i++) {
        code[ZSTD_LL][i] = (uint8_t)ll_code(seq[i].lit_len);
        code[ZSTD_ML][i] = (uint8_t)ml_code(seq[i].match_len);
        code[ZSTD_OF][i] = (uint8_t)highbit(seq[i].offset_value);
        for (int k = 0; k < 3; k++)
            count[k][code[k][i]]++;
    }
    static const unsigned nsyms[3] = {ZSTD_LL_CODES, ZSTD_OF_CODES, ZSTD_ML_CODES};
    seq_coder *sc = malloc(3 * sizeof *sc);
    err = sc ? SQ_OK : SQ_ERR_MEM;
    for (int k = 0; !err && k < 3; k++)
        err = choose_table(&sc[k], k, count[k], nsyms[k], nseq);
    if (!err)
        err = sq_buf_push(out, (uint8_t)(sc[0].mode << 6 | sc[1].mode << 4 | sc[2].mode << 2));
    for (int k = 0; !err && k < 3; k++)
        err = sq_buf_append(out, sc[k].desc.data, sc[k].desc.len);

    if (!err) {
        /* Backwards: the last sequence seeds the states, extra bits are
         * written LL, ML, OF and states advance OF, ML, LL — the reverse
         * of the decoder's order. */
        bitwriter_lsb bw;
        bwl_init(&bw, out);
        fse_cstate s[3];
        size_t i = nseq - 1;
        for (int k = 0; k < 3; k++) {
            static const int init_order[3] = {ZSTD_ML, ZSTD_OF, ZSTD_LL};
            int t = init_order[k];
            if (sc[t].t.log)
                fse_cstate_init(&s[t], &sc[t].t, code[t][i]);
            else
                s[t] = (fse_cstate){0, &sc[t].t};
        }
        for (;;) {
            const zseq *q = &seq[i];
            unsigned llc = code[ZSTD_LL][i], mlc = code[ZSTD_ML][i], ofc = code[ZSTD_OF][i];
            bwl_put(&bw, q->lit_len - zstd_ll_base[llc], zstd_ll_bits[llc]);
            bwl_put(&bw, q->match_len - zstd_ml_base[mlc], zstd_ml_bits[mlc]);
            bwl_put(&bw, q->offset_value - (1u << ofc), ofc);
            if (i-- == 0)
                break;
            encode_sym(&s[ZSTD_OF], &bw, code[ZSTD_OF][i]);
            encode_sym(&s[ZSTD_ML], &bw, code[ZSTD_ML][i]);
            encode_sym(&s[ZSTD_LL], &bw, code[ZSTD_LL][i]);
        }
        static const int flush_order[3] = {ZSTD_ML, ZSTD_OF, ZSTD_LL};
        for (int k = 0; k < 3; k++)
            if (s[flush_order[k]].t->log)
                fse_cstate_flush(&s[flush_order[k]], &bw);
        err = bwl_close_rev(&bw);
    }
    for (int k = 0; k < 3; k++)
        free(code[k]);
    if (sc)
        for (int k = 0; k < 3; k++)
            sq_buf_free(&sc[k].desc);
    free(sc);
    return err;
}

/* ---- blocks and frame ------------------------------------------------ */

static int put_block_header(sq_buf *out, int last, unsigned type, size_t size)
{
    return sq_buf_put_le(out, (uint32_t)last | type << 1 | (uint32_t)size << 3, 3);
}

static int write_block(zstd_cctx *c, sq_buf *out, size_t start, size_t end, int last)
{
    const uint8_t *p = c->in + start;
    size_t len = end - start, i = 1;
    while (i < len && p[i] == p[0])
        i++;
    /* Raw and RLE blocks leave the decoder's repeat offsets alone, so the
     * parse's updates are undone if the block is not sent compressed. */
    uint32_t saved[3];
    memcpy(saved, c->rep, sizeof saved);
    parse_block(c, start, end);  /* also indexes the block for later ones */
    if (len > 0 && i == len) {
        memcpy(c->rep, saved, sizeof saved);
        int err = put_block_header(out, last, ZSTD_BLOCK_RLE, len);
        return err ? err : sq_buf_push(out, p[0]);
    }
    size_t at = out->len;
    int err = put_block_header(out, last, ZSTD_BLOCK_COMPRESSED, 0);
    if (!err)
        err = write_literals(out, c->lit, c->nlit);
    if (!err)
        err = write_sequences(out, c->seq, c->nseq);
    if (err)
        return err;
    size_t size = out->len - at - 3;
    if (size >= len) {
        memcpy(c->rep, saved, sizeof saved);
        out->len = at;
        err = put_block_header(out, last, ZSTD_BLOCK_RAW, len);
        return err ? err : sq_buf_append(out, p, len);
    }
    uint32_t h = (uint32_t)last | ZSTD_BLOCK_COMPRESSED << 1 | (uint32_t)size << 3;
    for (int k = 0; k < 3; k++)
        out->data[at + (size_t)k] = (uint8_t)(h >> (8 * k));
    return SQ_OK;
}

int sq_zstd_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    if (level == 0)
        level = 3;
    if (level < 1 || level > 19)
        return SQ_ERR_ARG;
    unsigned wlog = levels[level].window_log;
    while (wlog > 10 && ((size_t)1 << (wlog - 1)) >= n)
        wlog--;  /* no point in a window larger than the input */
    zstd_cctx *c = calloc(1, sizeof *c);
    if (!c)
        return SQ_ERR_MEM;
    c->in = in;
    c->n = n;
    c->lazy = levels[level].lazy;
    c->rep[0] = 1, c->rep[1] = 4, c->rep[2] = 8;
    c->params = (lz_params){
        .window = 1u << wlog, .max_dist = (1u << wlog) - 1,
        .min_match = ZSTD_MIN_MATCH, .max_match = ZSTD_MAX_MATCH,
        .max_chain = levels[level].chain, .nice_len = 256,
        .hash_bits = wlog < 20 ? wlog : 20,
    };
    c->seq = malloc(sizeof(zseq) * (ZSTD_BLOCK_MAX / ZSTD_MIN_MATCH + 1));
    c->lit = malloc(ZSTD_BLOCK_MAX);
    int err = c->seq && c->lit ? lz_matcher_init(&c->finder, in, n, &c->params) : SQ_ERR_MEM;

    /* Frame header: single segment (window = content size) when the input
     * fits the window, else a window descriptor; content size; checksum. */
    int single = n <= ((size_t)1 << wlog);
    unsigned fcs_code = n < 256 && single ? 0 : n < 65536 + 256 ? 1 : n <= 0xFFFFFFFFu ? 2 : 3;
    static const uint8_t fcs_len[4] = {1, 2, 4, 8};
    if (!err)
        err = sq_buf_put_le(out, ZSTD_MAGIC, 4);
    if (!err)
        err = sq_buf_push(out, (uint8_t)(fcs_code << 6 | single << 5 | 1u << 2));
    if (!err && !single)
        err = sq_buf_push(out, (uint8_t)((wlog - 10) << 3));
    if (!err)
        err = sq_buf_put_le(out, fcs_code == 1 ? n - 256 : n, fcs_len[fcs_code]);

    size_t pos = 0;
    do {  /* at least one block, even for empty input */
        size_t end = n - pos < ZSTD_BLOCK_MAX ? n : pos + ZSTD_BLOCK_MAX;
        if (!err)
            err = write_block(c, out, pos, end, end == n);
        pos = end;
    } while (!err && pos < n);
    if (!err)
        err = sq_buf_put_le(out, (uint32_t)sq_xxh64(in, n, 0), 4);
    if (c->finder.head)
        lz_matcher_free(&c->finder);
    free(c->seq);
    free(c->lit);
    free(c);
    return err;
}
