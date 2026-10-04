/* gzip container (RFC 1952): header, DEFLATE data, CRC-32 and size.
 * A .gz file may hold several members back to back; their outputs are
 * concatenated, as `gzip -d` does. */
#include "checksum.h"
#include "deflate.h"

enum { FTEXT = 1, FHCRC = 2, FEXTRA = 4, FNAME = 8, FCOMMENT = 16 };

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int sq_gzip_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    if (level == 0)
        level = 6;
    /* ID1 ID2 CM FLG MTIME(4) XFL OS. MTIME 0 = unknown, keeping output
     * reproducible; XFL 2/4 = max/fastest; OS 3 = Unix. */
    uint8_t xfl = level == 9 ? 2 : level == 1 ? 4 : 0;
    const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, xfl, 3};
    int err = sq_buf_append(out, hdr, sizeof hdr);
    if (!err)
        err = sq_deflate_compress(in, n, level, out);
    if (!err)
        err = sq_buf_put_le(out, sq_crc32(0, in, n), 4);
    if (!err)
        err = sq_buf_put_le(out, (uint32_t)n, 4);  /* size mod 2^32 */
    return err;
}

/* Skips a zero-terminated field; returns the new offset or 0 on overrun. */
static size_t skip_string(const uint8_t *in, size_t n, size_t pos)
{
    while (pos < n && in[pos])
        pos++;
    return pos < n ? pos + 1 : 0;
}

static int member(const uint8_t *in, size_t n, sq_buf *out, size_t *used)
{
    if (n < 18 || in[0] != 0x1f || in[1] != 0x8b)
        return SQ_ERR_DATA;
    if (in[2] != 8 || (in[3] & 0xE0))
        return SQ_ERR_DATA;  /* unknown method or reserved flag bits */
    unsigned flg = in[3];
    size_t pos = 10;
    if (flg & FEXTRA) {
        size_t xlen = in[pos] | (size_t)in[pos + 1] << 8;
        pos += 2 + xlen;
        if (pos > n)
            return SQ_ERR_DATA;
    }
    if ((flg & FNAME) && !(pos = skip_string(in, n, pos)))
        return SQ_ERR_DATA;
    if ((flg & FCOMMENT) && !(pos = skip_string(in, n, pos)))
        return SQ_ERR_DATA;
    if (flg & FHCRC) {
        if (pos + 2 > n)
            return SQ_ERR_DATA;
        /* The header CRC is the low 16 bits of the CRC-32 of the header. */
        if ((sq_crc32(0, in, pos) & 0xFFFF) != (in[pos] | (unsigned)in[pos + 1] << 8))
            return SQ_ERR_DATA;
        pos += 2;
    }
    if (pos > n)
        return SQ_ERR_DATA;

    size_t start = out->len, len;
    int err = sq_inflate(in + pos, n - pos, out, &len);
    if (err)
        return err;
    pos += len;
    if (n - pos < 8)
        return SQ_ERR_DATA;
    size_t got = out->len - start;
    if (le32(in + pos) != sq_crc32(0, out->data + start, got) ||
        le32(in + pos + 4) != (uint32_t)got)
        return SQ_ERR_DATA;
    *used = pos + 8;
    return SQ_OK;
}

int sq_gzip_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    size_t pos = 0;
    do {
        size_t used;
        int err = member(in + pos, n - pos, out, &used);
        if (err)
            return err;
        pos += used;
    } while (pos < n);
    return SQ_OK;
}
