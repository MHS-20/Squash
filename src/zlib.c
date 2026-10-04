/* zlib container (RFC 1950): 2-byte header, DEFLATE data, Adler-32. */
#include "checksum.h"
#include "deflate.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int sq_zlib_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    if (level == 0)
        level = 6;
    /* CMF: method 8 (deflate), CINFO 7 (32 KB window). FLG carries a level
     * hint and FCHECK, which makes the 16-bit header a multiple of 31. */
    unsigned cmf = 0x78;
    unsigned flevel = level < 2 ? 0 : level < 6 ? 1 : level == 6 ? 2 : 3;
    unsigned flg = flevel << 6;
    flg += 31 - (cmf * 256 + flg) % 31;
    int err = sq_buf_push(out, (uint8_t)cmf);
    if (!err)
        err = sq_buf_push(out, (uint8_t)flg);
    if (!err)
        err = sq_deflate_compress(in, n, level, out);
    if (!err)
        err = sq_buf_put_be(out, sq_adler32(1, in, n), 4);
    return err;
}

int sq_zlib_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    if (n < 6)
        return SQ_ERR_DATA;
    unsigned cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 != 0)
        return SQ_ERR_DATA;
    if (flg & 0x20)
        return SQ_ERR_UNSUPPORTED;  /* preset dictionary */
    size_t start = out->len, used;
    int err = sq_inflate(in + 2, n - 2, out, &used);
    if (err)
        return err;
    if (n - 2 - used != 4)
        return SQ_ERR_DATA;
    if (be32(in + 2 + used) != sq_adler32(1, out->data + start, out->len - start))
        return SQ_ERR_DATA;
    return SQ_OK;
}
