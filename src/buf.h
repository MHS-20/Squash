/* Growable byte buffer: the output sink for every codec. */
#ifndef SQ_BUF_H
#define SQ_BUF_H

#include <stddef.h>
#include <stdint.h>

/* Hard ceiling on any single output. A corrupt stream can legally describe
 * an enormous expansion; refusing past this keeps decoders bounded. */
#define SQ_BUF_MAX ((size_t)1 << 32)

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} sq_buf;

void sq_buf_init(sq_buf *b);
void sq_buf_free(sq_buf *b);
int sq_buf_reserve(sq_buf *b, size_t extra);
int sq_buf_push(sq_buf *b, uint8_t byte);
int sq_buf_append(sq_buf *b, const void *src, size_t n);
int sq_buf_put_le(sq_buf *b, uint64_t v, int nbytes);
int sq_buf_put_be(sq_buf *b, uint64_t v, int nbytes);

/* Appends `len` bytes copied from `dist` bytes back: the LZ77 back-reference
 * shared by DEFLATE, LZ4 and Snappy. Overlap (dist < len) repeats a pattern.
 * `floor` is the earliest index a reference may reach (start of this stream
 * when several streams share one buffer). */
int sq_buf_lzcopy(sq_buf *b, size_t floor, size_t dist, size_t len);

#endif
