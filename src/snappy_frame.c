/* Snappy framing format (framing_format.txt): a stream identifier chunk,
 * then chunks of [type, 3-byte length, body]. Data chunks carry a masked
 * CRC-32C of their uncompressed bytes. There is no end marker. */
#include <string.h>

#include "checksum.h"
#include "snappy.h"

enum { CHUNK_COMPRESSED = 0x00, CHUNK_UNCOMPRESSED = 0x01, CHUNK_PADDING = 0xFE,
       CHUNK_STREAM_ID = 0xFF };

static const uint8_t stream_id[10] = {0xFF, 0x06, 0x00, 0x00, 's', 'N', 'a', 'P', 'p', 'Y'};

/* CRCs of data that itself contains CRCs are weak; rotating and adding a
 * constant keeps the stored value from being "a CRC of the data". */
static uint32_t masked_crc(const uint8_t *p, size_t n)
{
    uint32_t c = sq_crc32c(0, p, n);
    return ((c >> 15) | (c << 17)) + 0xA282EAD8u;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int put_chunk_header(sq_buf *out, uint8_t type, size_t len)
{
    int err = sq_buf_push(out, type);
    return err ? err : sq_buf_put_le(out, len, 3);
}

int sq_snappy_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    if (level < 0 || level > 1)
        return SQ_ERR_ARG;
    int err = sq_buf_append(out, stream_id, sizeof stream_id);
    for (size_t pos = 0; !err && pos < n; pos += SNAPPY_BLOCK) {
        size_t len = n - pos < SNAPPY_BLOCK ? n - pos : SNAPPY_BLOCK;
        size_t at = out->len;
        err = put_chunk_header(out, CHUNK_COMPRESSED, 0);
        if (!err)
            err = sq_buf_put_le(out, masked_crc(in + pos, len), 4);
        if (!err)
            err = sq_snappy_raw_compress(in + pos, len, 0, out);
        if (err)
            break;
        size_t body = out->len - at - 4;
        /* Store raw unless compression saves at least 1/8: a raw chunk
         * decodes as a plain copy, so small savings are not worth it. */
        if (body - 4 >= len - len / 8) {
            out->len = at;
            err = put_chunk_header(out, CHUNK_UNCOMPRESSED, len + 4);
            if (!err)
                err = sq_buf_put_le(out, masked_crc(in + pos, len), 4);
            if (!err)
                err = sq_buf_append(out, in + pos, len);
        } else {
            for (int i = 0; i < 3; i++)
                out->data[at + 1 + i] = (uint8_t)(body >> (8 * i));
        }
    }
    return err;
}

int sq_snappy_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    if (n == 0)
        return SQ_OK;  /* the reference tools write nothing for empty input */
    if (n < sizeof stream_id || memcmp(in, stream_id, sizeof stream_id) != 0)
        return SQ_ERR_DATA;
    size_t pos = sizeof stream_id;
    while (pos < n) {
        if (n - pos < 4)
            return SQ_ERR_DATA;
        uint8_t type = in[pos];
        size_t len = in[pos + 1] | (size_t)in[pos + 2] << 8 | (size_t)in[pos + 3] << 16;
        pos += 4;
        if (len > n - pos)
            return SQ_ERR_DATA;
        const uint8_t *body = in + pos;
        pos += len;
        if (type == CHUNK_STREAM_ID) {
            /* Repeats are allowed: concatenated streams are one stream. */
            if (len != 6 || memcmp(body, stream_id + 4, 6) != 0)
                return SQ_ERR_DATA;
            continue;
        }
        if (type == CHUNK_PADDING || type >= 0x80)
            continue;  /* padding and reserved skippable chunks */
        if (type != CHUNK_COMPRESSED && type != CHUNK_UNCOMPRESSED)
            return SQ_ERR_DATA;  /* reserved unskippable chunk */
        if (len < 4)
            return SQ_ERR_DATA;
        size_t start = out->len;
        int err;
        if (type == CHUNK_COMPRESSED) {
            err = snappy_raw_decode(body + 4, len - 4, out, SNAPPY_BLOCK);
        } else {
            err = len - 4 > SNAPPY_BLOCK ? SQ_ERR_DATA : sq_buf_append(out, body + 4, len - 4);
        }
        if (err)
            return err;
        if (masked_crc(out->data + start, out->len - start) != le32(body))
            return SQ_ERR_DATA;
    }
    return SQ_OK;
}
