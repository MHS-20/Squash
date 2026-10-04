/* LZ4 internals: the raw block format, shared by the frame and legacy
 * containers (and usable on its own when sizes are known out of band). */
#ifndef SQ_LZ4_H
#define SQ_LZ4_H

#include "lz77.h"
#include "squash.h"

#define LZ4_MIN_MATCH 4
#define LZ4_MAX_DIST 65535
#define LZ4_LAST_LITERALS 5  /* the last 5 bytes are always literals */
#define LZ4_MFLIMIT 12       /* the last match starts >= 12 bytes before the end */

/* Encodes `in` as one block. Level 1..9 trades speed for ratio. */
int lz4_block_compress(const uint8_t *in, size_t n, int level, sq_buf *out);

/* Decodes one block of `n` bytes. References may reach back to `floor` in
 * `out` (earlier blocks of a linked frame); the block may add at most
 * `max_out` bytes. */
int lz4_block_decompress(const uint8_t *in, size_t n, sq_buf *out, size_t floor, size_t max_out);

#endif
