/* Registry of formats: lets the CLI and the tests treat every codec alike. */
#ifndef SQ_CODEC_H
#define SQ_CODEC_H

#include "squash.h"

typedef struct {
    const char *name;
    const char *ext;          /* file suffix, including the dot */
    const char *magic;        /* leading bytes for auto-detection, or NULL */
    size_t magic_len;
    int min_level, max_level; /* 0 always means "default" as well */
    int (*compress)(const uint8_t *in, size_t n, int level, sq_buf *out);
    int (*decompress)(const uint8_t *in, size_t n, sq_buf *out);
} sq_codec;

extern const sq_codec sq_codecs[];
extern const size_t sq_codec_count;

const sq_codec *sq_codec_find(const char *name);
/* Format whose magic number starts `in`, or NULL. */
const sq_codec *sq_codec_detect(const uint8_t *in, size_t n);

#endif
