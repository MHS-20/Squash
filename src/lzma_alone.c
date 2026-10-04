/* The .lzma ("LZMA_Alone") container: a 13-byte header — properties byte,
 * 32-bit dictionary size, 64-bit uncompressed size (all ones = unknown,
 * in which case the stream must end with an end marker) — then the raw
 * range-coded LZMA stream. */
#include "lzma.h"

#define UNKNOWN_SIZE UINT64_MAX

int sq_lzma_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    lzma_encoder e;
    int err = lzma_encoder_init(&e, in, n, level);
    if (err)
        return err;
    err = sq_buf_push(out, (uint8_t)((e.m.pb * 5 + e.m.lp) * 9 + e.m.lc));
    if (!err)
        err = sq_buf_put_le(out, lzma_encoder_dict_size(&e), 4);
    if (!err)
        err = sq_buf_put_le(out, n, 8);  /* known size: no end marker */
    rc_encoder rc;
    rc_enc_init(&rc, out);
    while (!err && e.pos < n)
        lzma_encode_step(&e, &rc);
    if (!err)
        err = rc_enc_flush(&rc);
    lzma_encoder_free(&e);
    return err;
}

int sq_lzma_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    if (n < 13)
        return SQ_ERR_DATA;
    unsigned lc, lp, pb;
    if (lzma_props_decode(in[0], &lc, &lp, &pb))
        return SQ_ERR_DATA;
    uint64_t size = 0;
    for (int i = 7; i >= 0; i--)
        size = size << 8 | in[5 + i];
    size_t start = out->len;
    if (size != UNKNOWN_SIZE && size > SQ_BUF_MAX - start)
        return SQ_ERR_MEM;
    size_t target = size == UNKNOWN_SIZE ? SQ_BUF_MAX : start + (size_t)size;

    lzma_model m;
    int err = lzma_model_init(&m, lc, lp, pb);
    if (err)
        return err;
    rc_decoder rc;
    int ended = 0;
    err = rc_dec_init(&rc, in + 13, n - 13);
    if (!err)
        err = lzma_decode(&m, &rc, out, start, target, 1, &ended);
    /* With an unknown size only the end marker may stop the stream. */
    if (!err && size == UNKNOWN_SIZE && !ended)
        err = SQ_ERR_DATA;
    if (!err && (rc.overrun || rc.code != 0))
        err = SQ_ERR_DATA;  /* the coder must finish exactly */
    lzma_model_free(&m);
    return err;
}
