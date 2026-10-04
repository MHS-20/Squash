/* Public API of libsquash: whole-buffer compression and decompression. */
#ifndef SQUASH_H
#define SQUASH_H

#include <stddef.h>
#include <stdint.h>

#include "buf.h"

/* Every function returns SQ_OK or a negative error. Decoders never trust
 * their input: any malformed stream maps to SQ_ERR_DATA. */
enum {
    SQ_OK = 0,
    SQ_ERR_DATA = -1,        /* corrupt, truncated or inconsistent stream */
    SQ_ERR_MEM = -2,         /* allocation failed or output limit hit */
    SQ_ERR_ARG = -3,         /* bad parameter (e.g. level out of range) */
    SQ_ERR_UNSUPPORTED = -4, /* valid stream using a feature not implemented */
};

const char *sq_strerror(int err);

/* Compressors append to `out`; decompressors append the decoded bytes.
 * `level` is codec specific; 0 means "the codec default". */
int sq_deflate_compress(const uint8_t *in, size_t n, int level, sq_buf *out);
int sq_deflate_decompress(const uint8_t *in, size_t n, sq_buf *out);
int sq_zlib_compress(const uint8_t *in, size_t n, int level, sq_buf *out);
int sq_zlib_decompress(const uint8_t *in, size_t n, sq_buf *out);
int sq_gzip_compress(const uint8_t *in, size_t n, int level, sq_buf *out);
int sq_gzip_decompress(const uint8_t *in, size_t n, sq_buf *out);
int sq_lzw_compress(const uint8_t *in, size_t n, int level, sq_buf *out);
int sq_lzw_decompress(const uint8_t *in, size_t n, sq_buf *out);

#endif
