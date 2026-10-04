/* Snappy internals shared by the raw and framing formats. */
#ifndef SQ_SNAPPY_H
#define SQ_SNAPPY_H

#include "squash.h"

/* Snappy compresses in independent 64 KB fragments; the framing format
 * uses the same unit as its maximum chunk size. */
#define SNAPPY_BLOCK 65536

/* Raw decode, refusing streams whose declared length exceeds `max_len`. */
int snappy_raw_decode(const uint8_t *in, size_t n, sq_buf *out, uint64_t max_len);

#endif
