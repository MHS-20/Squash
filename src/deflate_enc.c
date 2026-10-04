/* DEFLATE encoder: LZ77 parse, then per block pick whichever of stored,
 * fixed-Huffman or dynamic-Huffman is smallest. */
#include <string.h>

#include "bitio.h"
#include "deflate.h"
#include "huffman.h"
#include "lz77.h"

/* Symbols per block. Smaller blocks adapt their codes to local statistics
 * but pay for a header each time; zlib's default buffer is the same size. */
#define BLOCK_SYMS 16384

/* Match-finder settings per level 1..9, taken from zlib's configuration
 * table: {nice, lazy, chain}. Levels 1-3 are greedy. */
static const struct {
    uint16_t nice, lazy, chain;
} levels[10] = {
    {0, 0, 0},
    {8, 0, 4}, {16, 0, 8}, {32, 0, 32},
    {16, 4, 16}, {32, 16, 32}, {128, 16, 128},
    {128, 32, 256}, {258, 128, 1024}, {258, 258, 4096},
};

static int len_code(uint32_t len)
{
    int c = 28;
    while (deflate_len_base[c] > len)
        c--;
    return c;
}

static int dist_code(uint32_t dist)
{
    int c = 29;
    while (deflate_dist_base[c] > dist)
        c--;
    return c;
}

/* A Huffman code ready for writing: lengths and bit-reversed codes. */
typedef struct {
    uint8_t len[288];
    uint32_t code[288];
} code_set;

static void make_codes(code_set *c, int n)
{
    uint32_t canon[288];
    huff_codes(c->len, n, canon);
    for (int i = 0; i < n; i++)
        c->code[i] = huff_reverse(canon[i], c->len[i]);
}

typedef struct {
    const lz_seq *seq;
    size_t k;           /* sequences in this block */
    size_t raw_len;     /* input bytes it covers */
    const uint8_t *raw; /* those bytes */
} block;

static void count_freqs(const block *b, uint32_t *lit, uint32_t *dist)
{
    memset(lit, 0, sizeof(uint32_t) * DEFLATE_NUM_LITLEN);
    memset(dist, 0, sizeof(uint32_t) * DEFLATE_NUM_DIST);
    const uint8_t *p = b->raw;
    for (size_t i = 0; i < b->k; i++) {
        const lz_seq *q = &b->seq[i];
        for (uint32_t j = 0; j < q->lit_len; j++)
            lit[*p++]++;
        if (q->match_len) {
            lit[257 + len_code(q->match_len)]++;
            dist[dist_code(q->dist)]++;
            p += q->match_len;
        }
    }
    lit[DEFLATE_END_BLOCK]++;
}

/* Bits for the block body (symbols plus extra bits) under given codes. */
static uint64_t body_bits(const uint32_t *lit, const uint32_t *dist,
                          const code_set *lc, const code_set *dc)
{
    uint64_t bits = 0;
    for (int i = 0; i < DEFLATE_NUM_LITLEN; i++)
        bits += (uint64_t)lit[i] * (lc->len[i] + (i > 256 ? deflate_len_extra[i - 257] : 0));
    for (int i = 0; i < DEFLATE_NUM_DIST; i++)
        bits += (uint64_t)dist[i] * (dc->len[i] + deflate_dist_extra[i]);
    return bits;
}

static void write_body(bitwriter_lsb *bw, const block *b, const code_set *lc, const code_set *dc)
{
    const uint8_t *p = b->raw;
    for (size_t i = 0; i < b->k; i++) {
        const lz_seq *q = &b->seq[i];
        for (uint32_t j = 0; j < q->lit_len; j++, p++)
            bwl_put(bw, lc->code[*p], lc->len[*p]);
        if (q->match_len) {
            int l = len_code(q->match_len), d = dist_code(q->dist);
            bwl_put(bw, lc->code[257 + l], lc->len[257 + l]);
            bwl_put(bw, q->match_len - deflate_len_base[l], deflate_len_extra[l]);
            bwl_put(bw, dc->code[d], dc->len[d]);
            bwl_put(bw, q->dist - deflate_dist_base[d], deflate_dist_extra[d]);
            p += q->match_len;
        }
    }
    bwl_put(bw, lc->code[DEFLATE_END_BLOCK], lc->len[DEFLATE_END_BLOCK]);
}

/* The dynamic header: run-length coded code lengths (symbols 16/17/18). */
typedef struct {
    int nlit, ndist, nclen;
    int n;                       /* RLE symbols */
    uint8_t sym[DEFLATE_NUM_LITLEN + DEFLATE_NUM_DIST];
    uint8_t extra[DEFLATE_NUM_LITLEN + DEFLATE_NUM_DIST];
    code_set cl;
    uint64_t bits;
} dyn_header;

static void rle_lengths(dyn_header *h, const uint8_t *lens, int total)
{
    h->n = 0;
    for (int i = 0; i < total;) {
        uint8_t l = lens[i];
        int run = 1;
        while (i + run < total && lens[i + run] == l)
            run++;
        i += run;
        if (l == 0) {
            while (run >= 11) {
                int r = run < 138 ? run : 138;
                h->sym[h->n] = 18, h->extra[h->n++] = (uint8_t)(r - 11);
                run -= r;
            }
            if (run >= 3) {
                h->sym[h->n] = 17, h->extra[h->n++] = (uint8_t)(run - 3);
                run = 0;
            }
        } else {
            /* 16 repeats the previous length, so the first is sent plainly. */
            h->sym[h->n] = l, h->extra[h->n++] = 0;
            run--;
            while (run >= 3) {
                int r = run < 6 ? run : 6;
                h->sym[h->n] = 16, h->extra[h->n++] = (uint8_t)(r - 3);
                run -= r;
            }
        }
        while (run-- > 0)
            h->sym[h->n] = l, h->extra[h->n++] = 0;
    }
}

static const uint8_t rle_extra_bits[19] = {[16] = 2, [17] = 3, [18] = 7};

static void build_dynamic(dyn_header *h, uint32_t *lit, uint32_t *dist, code_set *lc, code_set *dc)
{
    /* Like zlib, ensure two distance codes exist: some old inflaters reject
     * a distance code with fewer than two symbols. */
    int used = 0;
    for (int i = 0; i < DEFLATE_NUM_DIST; i++)
        used += dist[i] != 0;
    for (int i = 0; used < 2; i++)
        if (!dist[i])
            dist[i] = 1, used++;
    huff_lengths(lit, DEFLATE_NUM_LITLEN, 15, lc->len);
    huff_lengths(dist, DEFLATE_NUM_DIST, 15, dc->len);
    make_codes(lc, DEFLATE_NUM_LITLEN);
    make_codes(dc, DEFLATE_NUM_DIST);

    h->nlit = DEFLATE_NUM_LITLEN;
    while (h->nlit > 257 && lc->len[h->nlit - 1] == 0)
        h->nlit--;
    h->ndist = DEFLATE_NUM_DIST;
    while (h->ndist > 1 && dc->len[h->ndist - 1] == 0)
        h->ndist--;
    uint8_t lens[DEFLATE_NUM_LITLEN + DEFLATE_NUM_DIST];
    memcpy(lens, lc->len, (size_t)h->nlit);
    memcpy(lens + h->nlit, dc->len, (size_t)h->ndist);
    rle_lengths(h, lens, h->nlit + h->ndist);

    uint32_t cfreq[DEFLATE_NUM_CODELEN] = {0};
    for (int i = 0; i < h->n; i++)
        cfreq[h->sym[i]]++;
    huff_lengths(cfreq, DEFLATE_NUM_CODELEN, 7, h->cl.len);
    make_codes(&h->cl, DEFLATE_NUM_CODELEN);
    h->nclen = DEFLATE_NUM_CODELEN;
    while (h->nclen > 4 && h->cl.len[deflate_codelen_order[h->nclen - 1]] == 0)
        h->nclen--;

    h->bits = 5 + 5 + 4 + 3 * (uint64_t)h->nclen;
    for (int i = 0; i < h->n; i++)
        h->bits += h->cl.len[h->sym[i]] + rle_extra_bits[h->sym[i]];
}

static void write_dynamic_header(bitwriter_lsb *bw, const dyn_header *h)
{
    bwl_put(bw, (uint32_t)(h->nlit - 257), 5);
    bwl_put(bw, (uint32_t)(h->ndist - 1), 5);
    bwl_put(bw, (uint32_t)(h->nclen - 4), 4);
    for (int i = 0; i < h->nclen; i++)
        bwl_put(bw, h->cl.len[deflate_codelen_order[i]], 3);
    for (int i = 0; i < h->n; i++) {
        bwl_put(bw, h->cl.code[h->sym[i]], h->cl.len[h->sym[i]]);
        bwl_put(bw, h->extra[i], rle_extra_bits[h->sym[i]]);
    }
}

static void write_stored(bitwriter_lsb *bw, const block *b, int final)
{
    size_t left = b->raw_len;
    const uint8_t *p = b->raw;
    do {  /* at least once: an empty stored block is still a block */
        uint32_t len = left < 65535 ? (uint32_t)left : 65535;
        left -= len;
        bwl_put(bw, final && left == 0, 1);
        bwl_put(bw, 0, 2);
        bwl_flush(bw);  /* stored data starts on a byte boundary */
        bwl_put(bw, len, 16);
        bwl_put(bw, ~len & 0xFFFF, 16);
        for (uint32_t i = 0; i < len; i++)
            bwl_put(bw, *p++, 8);
    } while (left > 0);
}

static void write_block(bitwriter_lsb *bw, const block *b, int final)
{
    uint32_t lit[DEFLATE_NUM_LITLEN], dist[DEFLATE_NUM_DIST];
    count_freqs(b, lit, dist);

    code_set fixed_l, fixed_d;
    deflate_fixed_lengths(fixed_l.len, fixed_d.len);
    make_codes(&fixed_l, 288);
    make_codes(&fixed_d, 30);
    uint64_t fixed_bits = 3 + body_bits(lit, dist, &fixed_l, &fixed_d);

    dyn_header h;
    code_set dyn_l, dyn_d;
    build_dynamic(&h, lit, dist, &dyn_l, &dyn_d);
    uint64_t dyn_bits = 3 + h.bits + body_bits(lit, dist, &dyn_l, &dyn_d);
    uint64_t stored_bits = 8 * (b->raw_len + 5 * (b->raw_len / 65535 + 1)) + 7;

    if (stored_bits < fixed_bits && stored_bits < dyn_bits) {
        write_stored(bw, b, final);
    } else if (fixed_bits <= dyn_bits) {
        bwl_put(bw, final, 1);
        bwl_put(bw, 1, 2);
        write_body(bw, b, &fixed_l, &fixed_d);
    } else {
        bwl_put(bw, final, 1);
        bwl_put(bw, 2, 2);
        write_dynamic_header(bw, &h);
        write_body(bw, b, &dyn_l, &dyn_d);
    }
}

int sq_deflate_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    if (level == 0)
        level = 6;
    if (level < 1 || level > 9)
        return SQ_ERR_ARG;
    lz_params p = {
        .window = DEFLATE_WINDOW, .max_dist = DEFLATE_WINDOW,
        .min_match = 3, .max_match = DEFLATE_MAX_MATCH,
        .max_chain = levels[level].chain, .nice_len = levels[level].nice,
        .lazy = levels[level].lazy,
    };
    lz_seqs seqs;
    lz_seqs_init(&seqs);
    int err = lz_parse(in, n, &p, &seqs);
    if (err) {
        lz_seqs_free(&seqs);
        return err;
    }

    /* Cut the parse into blocks of about BLOCK_SYMS symbols; a long literal
     * run is split across blocks via `lit_off`. */
    static _Thread_local lz_seq part[BLOCK_SYMS];
    bitwriter_lsb bw;
    bwl_init(&bw, out);
    size_t i = 0, pos = 0;
    uint32_t lit_off = 0;
    int final;
    do {
        block b = {part, 0, 0, in + pos};
        size_t syms = 0;
        while (i < seqs.n && syms < BLOCK_SYMS) {
            const lz_seq *q = &seqs.seq[i];
            uint32_t lits = q->lit_len - lit_off;
            if (syms + lits > BLOCK_SYMS) {
                uint32_t take = (uint32_t)(BLOCK_SYMS - syms);
                part[b.k++] = (lz_seq){take, 0, 0};
                lit_off += take;
                b.raw_len += take;
                break;
            }
            part[b.k++] = (lz_seq){lits, q->match_len, q->dist};
            syms += lits + 1;
            b.raw_len += lits + q->match_len;
            lit_off = 0;
            i++;
        }
        final = i == seqs.n;
        write_block(&bw, &b, final);
        pos += b.raw_len;
    } while (!final);

    lz_seqs_free(&seqs);
    return bwl_flush(&bw);
}
