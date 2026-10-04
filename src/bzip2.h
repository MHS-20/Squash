/* bzip2 internals shared by the encoder and decoder. */
#ifndef SQ_BZIP2_H
#define SQ_BZIP2_H

#include "squash.h"

#define BZ_BLOCK_MAGIC 0x314159265359ull  /* BCD digits of pi */
#define BZ_END_MAGIC 0x177245385090ull    /* BCD digits of sqrt(pi) */
#define BZ_GROUP_SIZE 50                  /* symbols per selector */
#define BZ_MIN_TABLES 2
#define BZ_MAX_TABLES 6
#define BZ_MAX_SELECTORS (2 + 900000 / BZ_GROUP_SIZE)
#define BZ_MAX_ALPHA 258                  /* RUNA, RUNB, 255 MTF values, EOB */
#define BZ_MAX_CODE_LEN 20                /* decoders accept up to 20 */
#define BZ_ENC_CODE_LEN 17                /* bzip2 >= 1.0.3 writes at most 17 */

/* Toggles the randomisation pattern over a block (its own inverse). */
void bzip2_randomise(uint8_t *block, size_t n);

/* The encoder with the randomised-block option exposed, so tests can check
 * the decoder's derandomisation against the real bzip2. */
int bzip2_compress_ex(const uint8_t *in, size_t n, int level, int randomise, sq_buf *out);

#endif
