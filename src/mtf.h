/* Move-to-front: each byte becomes its index in a recency list, then moves
 * to the front. After a BWT, runs of equal bytes become runs of zeros. */
#ifndef SQ_MTF_H
#define SQ_MTF_H

#include <stddef.h>
#include <stdint.h>

/* The list starts as 0, 1, ..., 255 in both directions. */
void mtf_encode(const uint8_t *in, size_t n, uint8_t *out);
void mtf_decode(const uint8_t *in, size_t n, uint8_t *out);

#endif
