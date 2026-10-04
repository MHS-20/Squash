/* LZ4 block format. A block is a series of sequences:
 *
 *   token | [literal length bytes] | literals | offset (2, LE) | [match length bytes]
 *
 * The token's high nibble is the literal count, the low nibble the match
 * length minus 4; a nibble of 15 continues in following bytes, each added,
 * until one is below 255. The final sequence stops after its literals. */
#include "lz4.h"

/* Writes the part of a length that did not fit in its 4-bit nibble. */
static int put_length_tail(sq_buf *out, size_t len)
{
    int err = SQ_OK;
    for (; !err && len >= 255; len -= 255)
        err = sq_buf_push(out, 255);
    return err ? err : sq_buf_push(out, (uint8_t)len);
}

static const struct {
    uint16_t chain, lazy;
} levels[10] = {
    {0, 0}, {1, 0}, {2, 0}, {4, 0}, {8, 0}, {16, 0}, {16, 16}, {64, 32}, {256, 64}, {1024, 256},
};

int lz4_block_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    if (level == 0)
        level = 1;
    if (level < 1 || level > 9)
        return SQ_ERR_ARG;
    lz_params p = {
        .window = 65536, .max_dist = LZ4_MAX_DIST,
        .min_match = LZ4_MIN_MATCH, .max_match = 1u << 30,
        .max_chain = levels[level].chain, .nice_len = 1u << 30, .lazy = levels[level].lazy,
        .end_margin = LZ4_LAST_LITERALS, .start_margin = LZ4_MFLIMIT,
    };
    lz_seqs seqs;
    lz_seqs_init(&seqs);
    int err = lz_parse(in, n, &p, &seqs);
    const uint8_t *lit = in;
    for (size_t i = 0; !err && i < seqs.n; i++) {
        const lz_seq *q = &seqs.seq[i];
        uint32_t ml = q->match_len ? q->match_len - LZ4_MIN_MATCH : 0;
        uint8_t token = (uint8_t)((q->lit_len < 15 ? q->lit_len : 15) << 4 | (ml < 15 ? ml : 15));
        err = sq_buf_push(out, token);
        if (!err && q->lit_len >= 15)
            err = put_length_tail(out, q->lit_len - 15);
        if (!err)
            err = sq_buf_append(out, lit, q->lit_len);
        lit += q->lit_len + q->match_len;
        if (err || !q->match_len)
            continue;
        err = sq_buf_put_le(out, q->dist, 2);
        if (!err && ml >= 15)
            err = put_length_tail(out, ml - 15);
    }
    lz_seqs_free(&seqs);
    return err;
}

/* Reads a length continuation; returns 0 on running out of input. */
static int get_length_tail(const uint8_t **p, const uint8_t *end, size_t *len)
{
    uint8_t b;
    do {
        if (*p == end)
            return 0;
        b = *(*p)++;
        *len += b;
    } while (b == 255);
    return 1;
}

int lz4_block_decompress(const uint8_t *in, size_t n, sq_buf *out, size_t floor, size_t max_out)
{
    const uint8_t *p = in, *end = in + n;
    size_t limit = out->len + max_out;
    for (;;) {
        if (p == end)
            return SQ_ERR_DATA;  /* every block ends with a literal-only sequence */
        uint8_t token = *p++;
        size_t lit = token >> 4;
        if (lit == 15 && !get_length_tail(&p, end, &lit))
            return SQ_ERR_DATA;
        if (lit > (size_t)(end - p) || lit > limit - out->len)
            return SQ_ERR_DATA;
        int err = sq_buf_append(out, p, lit);
        if (err)
            return err;
        p += lit;
        if (p == end)
            return SQ_OK;
        if (end - p < 2)
            return SQ_ERR_DATA;
        size_t dist = p[0] | (size_t)p[1] << 8;
        p += 2;
        size_t ml = token & 15;
        if (ml == 15 && !get_length_tail(&p, end, &ml))
            return SQ_ERR_DATA;
        ml += LZ4_MIN_MATCH;
        if (ml > limit - out->len)
            return SQ_ERR_DATA;
        err = sq_buf_lzcopy(out, floor, dist, ml);  /* rejects dist 0 */
        if (err)
            return err;
    }
}
