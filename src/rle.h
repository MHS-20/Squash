/* The two run-length codings bzip2 uses. */
#ifndef SQ_RLE_H
#define SQ_RLE_H

#include <stddef.h>
#include <stdint.h>

#include "squash.h"

/* RLE1, applied to the raw input before the BWT: a run of 4 to 255 equal
 * bytes becomes the byte four times plus a count byte (0..251) of extra
 * repeats. It exists to keep long runs from slowing the original bzip2
 * block sort, and is now part of the format.
 *
 * Encodes from `in` while the output stays within `cap` bytes; `*used` gets
 * the input consumed (runs are never split across calls). */
int rle1_encode(const uint8_t *in, size_t n, size_t *used, sq_buf *out, size_t cap);
int rle1_decode(const uint8_t *in, size_t n, sq_buf *out);

/* RLE2, applied to MTF output: runs of zeros are written in bijective
 * base 2 with digits RUNA (=1) and RUNB (=2), least significant first;
 * any other MTF value v becomes symbol v + 1. */
#define RLE2_RUNA 0
#define RLE2_RUNB 1

/* `out` needs room for n symbols (the coding never expands). Returns the
 * symbol count. */
size_t rle2_encode(const uint8_t *in, size_t n, uint16_t *out);
/* Decodes symbols (no end-of-block) into at most `max` MTF values; returns
 * the count or SQ_ERR_DATA if a run or a symbol is out of range. */
int64_t rle2_decode(const uint16_t *in, size_t n, uint8_t *out, size_t max);

#endif
