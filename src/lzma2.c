/* LZMA2 (the xz filter): LZMA split into chunks, each either stored or
 * LZMA-compressed with its own range coder. A control byte per chunk
 * says which, and whether to reset the dictionary, the state, or the
 * properties:
 *   0x00          end of stream
 *   0x01 / 0x02   stored chunk, with / without dictionary reset
 *   0x80 | r<<5   LZMA chunk; r = 0 nothing, 1 state reset,
 *                 2 state reset + new properties, 3 also dictionary reset
 * Sizes are big-endian, minus one: up to 2 MiB uncompressed and 64 KiB
 * compressed per LZMA chunk, 64 KiB per stored chunk. */
#include "lzma.h"

#define CHUNK_UNCOMPRESSED_MAX (1u << 21)
#define CHUNK_COMPRESSED_MAX (1u << 16)
#define STORED_MAX (1u << 16)
/* Room left in a chunk for one more operation and the final flush. */
#define CHUNK_MARGIN 64

static int put_stored(sq_buf *out, const uint8_t *p, size_t n, int dict_reset)
{
    int err = SQ_OK;
    for (size_t pos = 0; !err && pos < n; pos += STORED_MAX) {
        size_t len = n - pos < STORED_MAX ? n - pos : STORED_MAX;
        err = sq_buf_push(out, dict_reset && pos == 0 ? 0x01 : 0x02);
        if (!err)
            err = sq_buf_put_be(out, len - 1, 2);
        if (!err)
            err = sq_buf_append(out, p + pos, len);
    }
    return err;
}

int lzma2_compress(const uint8_t *in, size_t n, int level, sq_buf *out, uint32_t *dict_size)
{
    lzma_encoder e;
    int err = lzma_encoder_init(&e, in, n, level);
    if (err)
        return err;
    *dict_size = lzma_encoder_dict_size(&e);
    sq_buf chunk;
    sq_buf_init(&chunk);
    int first = 1, need_props = 0, need_state_reset = 0;
    while (!err && e.pos < n) {
        size_t start = e.pos;
        rc_encoder rc;
        chunk.len = 0;
        rc_enc_init(&rc, &chunk);
        /* Fill the chunk while both size limits leave room for one more
         * operation (at most 273 bytes in, a few dozen bits out). */
        while (e.pos < n && e.pos - start <= CHUNK_UNCOMPRESSED_MAX - LZMA_MAX_MATCH &&
               chunk.len + rc.cache_size + 5 <= CHUNK_COMPRESSED_MAX - CHUNK_MARGIN)
            lzma_encode_step(&e, &rc);
        err = rc_enc_flush(&rc);
        if (err)
            break;
        size_t unc = e.pos - start;
        if (chunk.len >= unc) {
            /* Did not compress: store it. The encoder's model has moved on
             * while the decoder's will not, so the next LZMA chunk must
             * reset the state on both sides. */
            err = put_stored(out, in + start, unc, first);
            lzma_model_reset(&e.m);
            need_state_reset = 1;
            need_props |= first;  /* a dictionary reset demands new props */
            first = 0;
            continue;
        }
        unsigned reset = first ? 3 : need_props ? 2 : need_state_reset ? 1 : 0;
        uint8_t hdr[6] = {
            (uint8_t)(0x80 | reset << 5 | (unc - 1) >> 16), (uint8_t)((unc - 1) >> 8),
            (uint8_t)(unc - 1), (uint8_t)((chunk.len - 1) >> 8), (uint8_t)(chunk.len - 1),
            (uint8_t)((e.m.pb * 5 + e.m.lp) * 9 + e.m.lc)};
        err = sq_buf_append(out, hdr, reset >= 2 ? 6 : 5);
        if (!err)
            err = sq_buf_append(out, chunk.data, chunk.len);
        first = need_props = need_state_reset = 0;
    }
    if (!err)
        err = sq_buf_push(out, 0x00);
    sq_buf_free(&chunk);
    lzma_encoder_free(&e);
    return err;
}

int lzma2_decompress(const uint8_t *in, size_t n, sq_buf *out, size_t *used)
{
    lzma_model m = {.literal = NULL};
    int need_dict_reset = 1, need_props = 1, err = SQ_OK;
    size_t pos = 0, dict_start = out->len;
    for (;;) {
        if (pos >= n) {
            err = SQ_ERR_DATA;
            break;
        }
        unsigned control = in[pos++];
        if (control == 0x00)
            break;
        if (control >= 0xE0 || control == 0x01) {
            need_props = 1;
            need_dict_reset = 0;
            dict_start = out->len;
        } else if (need_dict_reset) {
            err = SQ_ERR_DATA;  /* the first chunk must reset the dictionary */
            break;
        }
        if (control < 0x80) {
            if (control > 0x02 || n - pos < 2) {
                err = SQ_ERR_DATA;
                break;
            }
            size_t len = ((size_t)in[pos] << 8 | in[pos + 1]) + 1;
            pos += 2;
            if (len > n - pos) {
                err = SQ_ERR_DATA;
                break;
            }
            if ((err = sq_buf_append(out, in + pos, len)))
                break;
            pos += len;
            continue;
        }
        if (n - pos < 4) {
            err = SQ_ERR_DATA;
            break;
        }
        size_t unc = ((size_t)(control & 0x1F) << 16 | (size_t)in[pos] << 8 | in[pos + 1]) + 1;
        size_t comp = ((size_t)in[pos + 2] << 8 | in[pos + 3]) + 1;
        pos += 4;
        if (control >= 0xC0) {
            unsigned lc, lp, pb;
            if (pos >= n || lzma_props_decode(in[pos++], &lc, &lp, &pb) || lc + lp > 4) {
                err = SQ_ERR_DATA;
                break;
            }
            lzma_model_free(&m);
            if ((err = lzma_model_init(&m, lc, lp, pb)))
                break;
            need_props = 0;
        } else if (need_props) {
            err = SQ_ERR_DATA;
            break;
        } else if (control >= 0xA0) {
            lzma_model_reset(&m);
        }
        if (comp > n - pos || unc > SQ_BUF_MAX - out->len) {
            err = SQ_ERR_DATA;
            break;
        }
        rc_decoder rc;
        int ended;
        err = rc_dec_init(&rc, in + pos, comp);
        if (!err)
            err = lzma_decode(&m, &rc, out, dict_start, out->len + unc, 0, &ended);
        /* The chunk's coder must end exactly at its compressed size. */
        if (!err && (rc.overrun || rc_dec_pos(&rc, in + pos) != comp || rc.code != 0))
            err = SQ_ERR_DATA;
        if (err)
            break;
        pos += comp;
    }
    lzma_model_free(&m);
    if (!err)
        *used = pos;
    return err;
}
