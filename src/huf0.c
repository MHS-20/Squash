#include "huf0.h"

#include <string.h>

#include "fse.h"
#include "huffman.h"

#define WEIGHT_FSE_MAX_LOG 6

static unsigned highbit(uint32_t v) { return 31 - (unsigned)__builtin_clz(v); }

/* ---- weights <-> lengths --------------------------------------------- */

/* Weight w > 0 means a code of max_bits + 1 - w bits. The last symbol's
 * weight is implicit: whatever completes the Kraft sum to a power of 2. */
static int weights_to_lengths(uint8_t *w, unsigned nw, uint8_t *length, unsigned *nsyms,
                              unsigned *max_bits)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < nw; i++) {
        if (w[i] > HUF0_MAX_BITS)
            return SQ_ERR_DATA;
        if (w[i])
            sum += 1u << (w[i] - 1);
    }
    if (sum == 0)
        return SQ_ERR_DATA;
    unsigned mb = highbit(sum) + 1;
    uint32_t rest = (1u << mb) - sum;
    if (mb > HUF0_MAX_BITS || (rest & (rest - 1)))
        return SQ_ERR_DATA;  /* the remainder must be a single power of 2 */
    w[nw] = (uint8_t)(highbit(rest) + 1);
    *nsyms = nw + 1;
    *max_bits = mb;
    for (unsigned i = 0; i <= nw; i++)
        length[i] = w[i] ? (uint8_t)(mb + 1 - w[i]) : 0;
    return SQ_OK;
}

/* Codes in ascending order starting from the longest lengths, ties by
 * symbol: the opposite of DEFLATE's canonical order. */
static void assign_codes(const uint8_t *length, unsigned nsyms, unsigned max_bits, uint32_t *code)
{
    uint32_t next = 0;
    for (unsigned l = max_bits; l >= 1; l--) {
        for (unsigned s = 0; s < nsyms; s++)
            if (length[s] == l)
                code[s] = next++;
        next >>= 1;
    }
}

/* ---- decoder --------------------------------------------------------- */

/* FSE-compressed weights: two states sharing one table, alternating, and
 * stopping when an update would read past the start of the stream. */
static int fse_weights(const uint8_t *in, size_t n, uint8_t *w, unsigned *nw)
{
    fse_norm norm[16];
    unsigned nsyms, log;
    size_t h = fse_read_ncount(in, n, norm, &nsyms, &log, WEIGHT_FSE_MAX_LOG, 16);
    static _Thread_local fse_dtable dt;
    if (!h || h >= n || fse_build_dtable(&dt, norm, nsyms, log))
        return SQ_ERR_DATA;
    bitreader_rev br;
    if (brr_init(&br, in + h, n - h))
        return SQ_ERR_DATA;
    fse_dstate s[2];
    fse_dstate_init(&s[0], &dt, &br);
    fse_dstate_init(&s[1], &dt, &br);
    if (br.overrun)
        return SQ_ERR_DATA;
    unsigned k = 0;
    for (int which = 0;; which ^= 1) {
        if (k >= HUF0_MAX_SYMS - 1)
            return SQ_ERR_DATA;
        w[k++] = (uint8_t)fse_peek(&s[which]);
        fse_update(&s[which], &br);
        if (br.overrun) {
            if (k >= HUF0_MAX_SYMS - 1)
                return SQ_ERR_DATA;
            w[k++] = (uint8_t)fse_peek(&s[which ^ 1]);
            break;
        }
    }
    *nw = k;
    return SQ_OK;
}

size_t huf0_read_table(huf0_dtable *t, const uint8_t *in, size_t n)
{
    if (n < 1)
        return 0;
    uint8_t w[HUF0_MAX_SYMS + 1] = {0};
    unsigned nw;
    size_t used;
    if (in[0] >= 128) {
        nw = in[0] - 127u;  /* 4-bit weights, two per byte, high nibble first */
        used = 1 + (nw + 1) / 2;
        if (used > n)
            return 0;
        for (unsigned i = 0; i < nw; i++)
            w[i] = (uint8_t)(i % 2 ? in[1 + i / 2] & 15 : in[1 + i / 2] >> 4);
    } else {
        used = 1 + (size_t)in[0];
        if (used > n || fse_weights(in + 1, in[0], w, &nw))
            return 0;
    }
    uint8_t length[HUF0_MAX_SYMS + 1];
    uint32_t code[HUF0_MAX_SYMS + 1];
    unsigned nsyms, mb;
    if (nw >= HUF0_MAX_SYMS || weights_to_lengths(w, nw, length, &nsyms, &mb))
        return 0;
    assign_codes(length, nsyms, mb, code);
    /* Codes are read MSB-first from the top of a max_bits peek. */
    for (unsigned s = 0; s < nsyms; s++) {
        if (!length[s])
            continue;
        uint32_t first = code[s] << (mb - length[s]), span = 1u << (mb - length[s]);
        for (uint32_t i = first; i < first + span; i++) {
            t->symbol[i] = (uint8_t)s;
            t->length[i] = length[s];
        }
    }
    t->max_bits = mb;
    return used;
}

int huf0_decode_stream(const huf0_dtable *t, const uint8_t *in, size_t n, uint8_t *out,
                       size_t count)
{
    bitreader_rev br;
    if (brr_init(&br, in, n))
        return SQ_ERR_DATA;
    for (size_t i = 0; i < count; i++) {
        /* Near the start the peek pads with zeros, which is how the last
         * short codes are decoded. */
        uint32_t v = (uint32_t)brr_peek(&br, t->max_bits);
        out[i] = t->symbol[v];
        brr_consume(&br, t->length[v]);
        if (br.overrun)
            return SQ_ERR_DATA;
    }
    return br.bits == 0 ? SQ_OK : SQ_ERR_DATA;
}

int huf0_decode_literals(const huf0_dtable *t, const uint8_t *in, size_t n, uint8_t *out,
                         size_t count, int four_streams)
{
    if (!four_streams)
        return huf0_decode_stream(t, in, n, out, count);
    if (n < 10)
        return SQ_ERR_DATA;
    size_t sz[4], per = (count + 3) / 4;
    sz[0] = in[0] | (size_t)in[1] << 8;
    sz[1] = in[2] | (size_t)in[3] << 8;
    sz[2] = in[4] | (size_t)in[5] << 8;
    if (sz[0] + sz[1] + sz[2] + 6 >= n)
        return SQ_ERR_DATA;
    sz[3] = n - 6 - sz[0] - sz[1] - sz[2];
    const uint8_t *p = in + 6;
    for (int k = 0; k < 4; k++) {
        size_t c = k < 3 ? per : count - 3 * per;
        if (3 * per > count && k == 3)
            return SQ_ERR_DATA;
        int err = huf0_decode_stream(t, p, sz[k], out + k * per, c);
        if (err)
            return err;
        p += sz[k];
    }
    return SQ_OK;
}

/* ---- encoder --------------------------------------------------------- */

int huf0_build(huf0_ctable *t, const uint32_t *count)
{
    unsigned used = 0, nsyms = 0;
    for (unsigned s = 0; s < HUF0_MAX_SYMS; s++)
        if (count[s])
            used++, nsyms = s + 1;
    if (used < 2)
        return SQ_ERR_ARG;
    huff_lengths(count, (int)nsyms, HUF0_MAX_BITS, t->length);
    unsigned mb = 0;
    for (unsigned s = 0; s < nsyms; s++)
        if (t->length[s] > mb)
            mb = t->length[s];
    t->nsyms = nsyms;
    t->max_bits = mb;
    assign_codes(t->length, nsyms, mb, t->code);
    return SQ_OK;
}

/* Weights of all symbols but the last, which the decoder infers. */
static unsigned table_weights(const huf0_ctable *t, uint8_t *w)
{
    for (unsigned s = 0; s + 1 < t->nsyms; s++)
        w[s] = t->length[s] ? (uint8_t)(t->max_bits + 1 - t->length[s]) : 0;
    return t->nsyms - 1;
}

/* FSE-compress the weights the way the decoder above expects: two states,
 * even indices on the first. Returns SQ_ERR_ARG if this representation
 * does not round-trip, so the caller can fall back. */
static int write_fse_weights(sq_buf *out, const uint8_t *w, unsigned nw)
{
    if (nw < 2)
        return SQ_ERR_ARG;
    uint32_t count[16] = {0};
    unsigned nsyms = 0;
    for (unsigned i = 0; i < nw; i++) {
        count[w[i]]++;
        if (w[i] + 1u > nsyms)
            nsyms = w[i] + 1u;
    }
    unsigned log = WEIGHT_FSE_MAX_LOG;
    fse_norm norm[16];
    static _Thread_local fse_ctable ct;
    sq_buf body;
    sq_buf_init(&body);
    int err = fse_normalize(count, nsyms, nw, log, 1, norm);
    if (!err)
        err = fse_write_ncount(&body, norm, nsyms, log);
    if (!err)
        err = fse_build_ctable(&ct, norm, nsyms, log);
    if (!err) {
        bitwriter_lsb bw;
        bwl_init(&bw, &body);
        fse_cstate s[2];
        /* The last two weights seed the states (free); the rest are
         * encoded backwards alternating, mirroring zstd's FSE encoder. */
        unsigned i = nw;
        if (nw & 1) {
            fse_cstate_init(&s[0], &ct, w[--i]);
            fse_cstate_init(&s[1], &ct, w[--i]);
            fse_encode(&s[0], &bw, w[--i]);
        } else {
            fse_cstate_init(&s[1], &ct, w[--i]);
            fse_cstate_init(&s[0], &ct, w[--i]);
        }
        while (i > 0) {
            fse_encode(&s[1], &bw, w[--i]);
            fse_encode(&s[0], &bw, w[--i]);
        }
        fse_cstate_flush(&s[1], &bw);
        fse_cstate_flush(&s[0], &bw);
        err = bwl_close_rev(&bw);
    }
    /* Check the decoder recovers exactly these weights: the stop rule
     * ("an update would overrun") is implicit and must hold. */
    uint8_t back[HUF0_MAX_SYMS + 1];
    unsigned nback;
    if (!err && (body.len > 127 || fse_weights(body.data, body.len, back, &nback) ||
                 nback != nw || memcmp(back, w, nw) != 0))
        err = SQ_ERR_ARG;
    if (!err)
        err = sq_buf_push(out, (uint8_t)body.len);
    if (!err)
        err = sq_buf_append(out, body.data, body.len);
    sq_buf_free(&body);
    return err;
}

int huf0_write_table(sq_buf *out, const huf0_ctable *t)
{
    uint8_t w[HUF0_MAX_SYMS];
    unsigned nw = table_weights(t, w);
    size_t mark = out->len;
    int err = write_fse_weights(out, w, nw);
    if (err != SQ_ERR_ARG)
        return err;
    out->len = mark;
    if (nw > 128)
        return SQ_ERR_ARG;  /* direct form holds at most 128 weights */
    err = sq_buf_push(out, (uint8_t)(127 + nw));
    for (unsigned i = 0; !err && i < nw; i += 2)
        err = sq_buf_push(out, (uint8_t)(w[i] << 4 | (i + 1 < nw ? w[i + 1] : 0)));
    return err;
}

static int encode_stream(sq_buf *out, const huf0_ctable *t, const uint8_t *in, size_t n)
{
    bitwriter_lsb bw;
    bwl_init(&bw, out);
    /* Backwards, so the decoder meets the first symbol first. */
    for (size_t i = n; i-- > 0; )
        bwl_put(&bw, t->code[in[i]], t->length[in[i]]);
    return bwl_close_rev(&bw);
}

int huf0_encode_literals(sq_buf *out, const huf0_ctable *t, const uint8_t *in, size_t n,
                         int four_streams)
{
    if (!four_streams)
        return encode_stream(out, t, in, n);
    size_t per = (n + 3) / 4, jump = out->len;
    int err = sq_buf_put_le(out, 0, 6);
    for (int k = 0; !err && k < 4; k++) {
        size_t start = out->len;
        size_t len = k < 3 ? per : n - 3 * per;
        err = encode_stream(out, t, in + k * per, len);
        if (!err && k < 3) {
            size_t sz = out->len - start;
            if (sz > 0xFFFF)
                return SQ_ERR_ARG;
            out->data[jump + 2 * k] = (uint8_t)sz;
            out->data[jump + 2 * k + 1] = (uint8_t)(sz >> 8);
        }
    }
    return err;
}
