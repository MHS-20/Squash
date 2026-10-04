/* Snappy raw format (format_description.txt): a varint uncompressed length,
 * then elements whose tag byte's low two bits select
 *   00 literal, 01 copy with 1-byte offset, 10 copy with 2-byte offset,
 *   11 copy with 4-byte offset. */
#include "lz77.h"
#include "snappy.h"

enum { TAG_LITERAL = 0, TAG_COPY1 = 1, TAG_COPY2 = 2, TAG_COPY4 = 3 };

static int put_literal(sq_buf *out, const uint8_t *p, size_t len)
{
    if (len == 0)
        return SQ_OK;
    size_t n = len - 1;
    int err;
    if (n < 60) {
        err = sq_buf_push(out, (uint8_t)(n << 2 | TAG_LITERAL));
    } else {
        /* Tags 60..63 say the length follows in 1..4 bytes. */
        int bytes = n < (1u << 8) ? 1 : n < (1u << 16) ? 2 : n < (1u << 24) ? 3 : 4;
        err = sq_buf_push(out, (uint8_t)((59 + bytes) << 2 | TAG_LITERAL));
        if (!err)
            err = sq_buf_put_le(out, n, bytes);
    }
    return err ? err : sq_buf_append(out, p, len);
}

/* One copy element of at most 64 bytes, in the shortest form that fits. */
static int put_copy_element(sq_buf *out, uint32_t dist, uint32_t len)
{
    if (len >= 4 && len < 12 && dist < 2048) {
        int err = sq_buf_push(out, (uint8_t)((dist >> 8) << 5 | (len - 4) << 2 | TAG_COPY1));
        return err ? err : sq_buf_push(out, (uint8_t)dist);
    }
    int err = sq_buf_push(out, (uint8_t)((len - 1) << 2 | TAG_COPY2));
    return err ? err : sq_buf_put_le(out, dist, 2);
}

static int put_copy(sq_buf *out, uint32_t dist, uint32_t len)
{
    int err = SQ_OK;
    /* Split long matches into 64-byte elements, keeping the remainder at
     * least 4 so it can still use the compact 1-byte-offset form. */
    while (!err && len >= 68) {
        err = put_copy_element(out, dist, 64);
        len -= 64;
    }
    if (!err && len > 64) {
        err = put_copy_element(out, dist, 60);
        len -= 60;
    }
    return err ? err : put_copy_element(out, dist, len);
}

int sq_snappy_raw_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    if (level < 0 || level > 1)
        return SQ_ERR_ARG;
    if (n > 0xFFFFFFFFu)
        return SQ_ERR_ARG;  /* the length preamble is 32-bit */
    int err = SQ_OK;
    for (uint64_t v = n; !err; v >>= 7) {
        err = sq_buf_push(out, (uint8_t)((v & 0x7F) | (v >= 0x80 ? 0x80 : 0)));
        if (v < 0x80)
            break;
    }
    /* Snappy's encoder is a fast greedy one with a short search; a chain of
     * 4 in our hash-chain parser is the closest equivalent. */
    lz_params p = {
        .window = SNAPPY_BLOCK, .max_dist = SNAPPY_BLOCK - 1,
        .min_match = 4, .max_match = 1u << 30,
        .max_chain = 4, .nice_len = 64, .lazy = 0,
    };
    lz_seqs seqs;
    lz_seqs_init(&seqs);
    for (size_t pos = 0; !err && pos < n; pos += SNAPPY_BLOCK) {
        size_t len = n - pos < SNAPPY_BLOCK ? n - pos : SNAPPY_BLOCK;
        seqs.n = 0;
        err = lz_parse(in + pos, len, &p, &seqs);
        const uint8_t *lit = in + pos;
        for (size_t i = 0; !err && i < seqs.n; i++) {
            const lz_seq *q = &seqs.seq[i];
            err = put_literal(out, lit, q->lit_len);
            if (!err && q->match_len)
                err = put_copy(out, q->dist, q->match_len);
            lit += q->lit_len + q->match_len;
        }
    }
    lz_seqs_free(&seqs);
    return err;
}

static uint32_t le(const uint8_t *p, int bytes)
{
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

int snappy_raw_decode(const uint8_t *in, size_t n, sq_buf *out, uint64_t max_len)
{
    const uint8_t *p = in, *end = in + n;
    uint64_t want = 0;
    for (int shift = 0;; shift += 7) {
        if (p == end || shift > 28)
            return SQ_ERR_DATA;  /* a 32-bit varint has at most 5 bytes */
        want |= (uint64_t)(*p & 0x7F) << shift;
        if (!(*p++ & 0x80))
            break;
    }
    if (want > 0xFFFFFFFFu || want > max_len)
        return SQ_ERR_DATA;
    size_t start = out->len;
    while (p < end) {
        uint8_t tag = *p++;
        size_t len, dist;
        int err;
        if ((tag & 3) == TAG_LITERAL) {
            len = (size_t)(tag >> 2) + 1;
            if (len > 60) {
                int bytes = (int)len - 60;
                if (end - p < bytes)
                    return SQ_ERR_DATA;
                len = (size_t)le(p, bytes) + 1;
                p += bytes;
            }
            if (len > (size_t)(end - p) || len > want - (out->len - start))
                return SQ_ERR_DATA;
            err = sq_buf_append(out, p, len);
            p += len;
        } else {
            int bytes = (tag & 3) == TAG_COPY1 ? 1 : (tag & 3) == TAG_COPY2 ? 2 : 4;
            if (end - p < bytes)
                return SQ_ERR_DATA;
            if ((tag & 3) == TAG_COPY1) {
                len = ((tag >> 2) & 7) + 4;
                dist = (size_t)(tag >> 5) << 8 | *p;
            } else {
                len = (size_t)(tag >> 2) + 1;
                dist = le(p, bytes);
            }
            p += bytes;
            if (len > want - (out->len - start))
                return SQ_ERR_DATA;
            err = sq_buf_lzcopy(out, start, dist, len);
        }
        if (err)
            return err;
    }
    return out->len - start == want ? SQ_OK : SQ_ERR_DATA;
}

int sq_snappy_raw_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    return snappy_raw_decode(in, n, out, 0xFFFFFFFFu);
}
