/* Burrows-Wheeler transform over cyclic rotations, as bzip2 uses it.
 * Output is the last column of the sorted rotation matrix plus `primary`,
 * the row holding the original string. */
#ifndef SQ_BWT_H
#define SQ_BWT_H

#include <stddef.h>
#include <stdint.h>

/* Sorts rotations directly with comparison sort: O(n^2 log n) in the worst
 * case. The definition made executable; used to check bwt_forward. */
int bwt_naive(const uint8_t *in, int32_t n, uint8_t *out, int32_t *primary);

/* Same result in linear time via the suffix array of in+in. */
int bwt_forward(const uint8_t *in, int32_t n, uint8_t *out, int32_t *primary);

/* Inverse transform; SQ_ERR_DATA if primary is out of range. */
int bwt_inverse(const uint8_t *in, int32_t n, int32_t primary, uint8_t *out);

#endif
