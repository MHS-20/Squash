#include "codec.h"

#include <string.h>

/* Raw formats without a magic number come last; detection skips them. */
const sq_codec sq_codecs[] = {
    {"gzip", ".gz", "\x1f\x8b", 2, 1, 9, sq_gzip_compress, sq_gzip_decompress},
    /* zlib's header is not a fixed magic; 0x78 covers every 32K-window
     * stream, which is all that exists in practice. */
    {"zlib", ".zz", "\x78", 1, 1, 9, sq_zlib_compress, sq_zlib_decompress},
    {"lzw", ".Z", "\x1f\x9d", 2, 9, 16, sq_lzw_compress, sq_lzw_decompress},
    {"lz4", ".lz4", "\x04\x22\x4d\x18", 4, 1, 9, sq_lz4_compress, sq_lz4_decompress},
    {"snappy", ".sz", "\xff\x06\x00\x00sNaPpY", 10, 1, 1, sq_snappy_compress, sq_snappy_decompress},
    {"bzip2", ".bz2", "BZh", 3, 1, 9, sq_bzip2_compress, sq_bzip2_decompress},
    {"deflate", ".deflate", NULL, 0, 1, 9, sq_deflate_compress, sq_deflate_decompress},
    {"snappy-raw", ".snappy", NULL, 0, 1, 1, sq_snappy_raw_compress, sq_snappy_raw_decompress},
};
const size_t sq_codec_count = sizeof sq_codecs / sizeof sq_codecs[0];

const sq_codec *sq_codec_find(const char *name)
{
    for (size_t i = 0; i < sq_codec_count; i++)
        if (strcmp(sq_codecs[i].name, name) == 0)
            return &sq_codecs[i];
    return NULL;
}

const sq_codec *sq_codec_detect(const uint8_t *in, size_t n)
{
    for (size_t i = 0; i < sq_codec_count; i++) {
        const sq_codec *c = &sq_codecs[i];
        if (c->magic && n >= c->magic_len && memcmp(in, c->magic, c->magic_len) == 0)
            return c;
    }
    return NULL;
}
