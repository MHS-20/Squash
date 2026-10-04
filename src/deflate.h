/* DEFLATE internals shared by the raw, zlib and gzip front ends. */
#ifndef SQ_DEFLATE_H
#define SQ_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

#include "squash.h"

#define DEFLATE_NUM_LITLEN 286  /* 0-255 literals, 256 end, 257-285 lengths */
#define DEFLATE_NUM_DIST 30
#define DEFLATE_NUM_CODELEN 19
#define DEFLATE_END_BLOCK 256
#define DEFLATE_WINDOW 32768
#define DEFLATE_MAX_MATCH 258

/* RFC 1951 section 3.2.5: a length or distance is sent as a code that picks
 * a base value, plus that many raw extra bits added to the base. */
extern const uint16_t deflate_len_base[29];
extern const uint8_t deflate_len_extra[29];
extern const uint16_t deflate_dist_base[30];
extern const uint8_t deflate_dist_extra[30];
/* Order in which code-length code lengths are sent (most likely first, so
 * trailing zeros can be trimmed). */
extern const uint8_t deflate_codelen_order[19];

/* Fixed Huffman code lengths (block type 1), RFC 1951 section 3.2.6. */
void deflate_fixed_lengths(uint8_t litlen[288], uint8_t dist[30]);

/* Decode one raw DEFLATE stream from `in`, appending to `out`. `*used` gets
 * the number of input bytes consumed, so containers can find their trailer. */
int sq_inflate(const uint8_t *in, size_t n, sq_buf *out, size_t *used);

#endif
