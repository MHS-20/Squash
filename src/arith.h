/* Multi-symbol arithmetic coding in the Witten-Neal-Cleary (1987) integer
 * form, with an order-0 static model and an order-0 adaptive model.
 *
 * These are teaching codecs, not file formats: the static stream is
 * [8-byte length][256 x 2-byte frequencies][code bits], the adaptive one
 * [8-byte length][code bits]. */
#ifndef SQ_ARITH_H
#define SQ_ARITH_H

#include "squash.h"

int arith_static_compress(const uint8_t *in, size_t n, sq_buf *out);
int arith_static_decompress(const uint8_t *in, size_t n, sq_buf *out);
int arith_adaptive_compress(const uint8_t *in, size_t n, sq_buf *out);
int arith_adaptive_decompress(const uint8_t *in, size_t n, sq_buf *out);

#endif
