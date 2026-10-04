/* Range asymmetric numeral systems (rANS), byte-renormalised as in
 * ryg_rans: the whole coder state is one integer x. Encoding symbol s with
 * frequency f (out of 2^scale) maps x to about x * 2^scale / f; decoding
 * inverts it exactly. Like a stack, the last symbol encoded is the first
 * decoded, so the encoder runs over the input backwards.
 *
 * Teaching codecs with an ad-hoc container:
 * [8-byte length][256 x 2-byte frequencies][state(s)][bytes]. The
 * interleaved variant alternates two independent states over the symbols,
 * which a CPU can decode in parallel. */
#ifndef SQ_RANS_H
#define SQ_RANS_H

#include "squash.h"

#define RANS_SCALE_BITS 12  /* frequencies are out of 4096 */

int rans_compress(const uint8_t *in, size_t n, sq_buf *out);
int rans_decompress(const uint8_t *in, size_t n, sq_buf *out);
int rans2_compress(const uint8_t *in, size_t n, sq_buf *out);
int rans2_decompress(const uint8_t *in, size_t n, sq_buf *out);

#endif
