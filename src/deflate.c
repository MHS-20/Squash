/* Raw DEFLATE front end: the bare RFC 1951 stream with no container. */
#include "deflate.h"

int sq_deflate_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    size_t used;
    int err = sq_inflate(in, n, out, &used);
    if (!err && used != n)
        return SQ_ERR_DATA;  /* trailing bytes after the final block */
    return err;
}
