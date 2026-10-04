/* zstd's Huffman coding of literals (RFC 8878 sections 4.2 and 3.1.1.3.1):
 * code lengths sent as "weights", prefix codes assigned longest first,
 * and streams read backwards, one or four at a time. */
#ifndef SQ_HUF0_H
#define SQ_HUF0_H

#include "bitio.h"
#include "squash.h"

#define HUF0_MAX_BITS 11
#define HUF0_MAX_SYMS 256

typedef struct {
    uint8_t symbol[1 << HUF0_MAX_BITS];  /* indexed by the next max_bits bits */
    uint8_t length[1 << HUF0_MAX_BITS];
    unsigned max_bits;
} huf0_dtable;

/* Reads a tree description; returns bytes consumed, 0 on error. */
size_t huf0_read_table(huf0_dtable *t, const uint8_t *in, size_t n);
/* Decodes `count` symbols from one backward stream that must be consumed
 * exactly. */
int huf0_decode_stream(const huf0_dtable *t, const uint8_t *in, size_t n, uint8_t *out,
                       size_t count);
/* One stream, or four with a 6-byte jump table, per the literals header. */
int huf0_decode_literals(const huf0_dtable *t, const uint8_t *in, size_t n, uint8_t *out,
                         size_t count, int four_streams);

/* ---- encoder ---- */

typedef struct {
    uint8_t length[HUF0_MAX_SYMS];
    uint32_t code[HUF0_MAX_SYMS];
    unsigned nsyms;      /* last used symbol + 1 */
    unsigned max_bits;
} huf0_ctable;

/* Code lengths and codes for `count`; SQ_ERR_ARG if fewer than two
 * symbols are used (zstd uses RLE literals for that). */
int huf0_build(huf0_ctable *t, const uint32_t *count);
int huf0_write_table(sq_buf *out, const huf0_ctable *t);
int huf0_encode_literals(sq_buf *out, const huf0_ctable *t, const uint8_t *in, size_t n,
                         int four_streams);

#endif
