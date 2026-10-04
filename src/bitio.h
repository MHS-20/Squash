/* Bit-level readers and writers in both packing orders.
 *
 * LSB-first (DEFLATE, LZW): the first bit of the stream is bit 0 of byte 0,
 * and multi-bit values are stored least significant bit first.
 * MSB-first (bzip2): the first bit is bit 7 of byte 0, and values are stored
 * most significant bit first.
 *
 * Readers keep up to 64 bits buffered. Past the end of input they feed zero
 * bits and set `overrun` once a consumed bit did not exist, so decoders can
 * decode the final symbols with a plain peek and check for truncation once. */
#ifndef SQ_BITIO_H
#define SQ_BITIO_H

#include <stddef.h>
#include <stdint.h>

#include "squash.h"

typedef struct {
    const uint8_t *p, *end;
    uint64_t bits;   /* next bit to read is bit 0 */
    unsigned count;  /* valid bits in `bits` */
    int overrun;
} bitreader_lsb;

typedef struct {
    const uint8_t *p, *end;
    uint64_t bits;   /* next bit to read is bit 63 */
    unsigned count;
    int overrun;
} bitreader_msb;

typedef struct {
    sq_buf *out;
    uint64_t bits;
    unsigned count;
    int err;         /* sticky: first buffer error, checked at flush */
} bitwriter_lsb;

typedef struct {
    sq_buf *out;
    uint64_t bits;   /* pending bits sit at the top */
    unsigned count;
    int err;
} bitwriter_msb;

void brl_init(bitreader_lsb *br, const uint8_t *p, size_t n);
void brm_init(bitreader_msb *br, const uint8_t *p, size_t n);
void bwl_init(bitwriter_lsb *bw, sq_buf *out);
void bwm_init(bitwriter_msb *bw, sq_buf *out);
/* Pad the final partial byte with zeros; returns the sticky error. */
int bwl_flush(bitwriter_lsb *bw);
int bwm_flush(bitwriter_msb *bw);

/* ---- LSB-first reader ------------------------------------------------ */

static inline void brl_refill(bitreader_lsb *br)
{
    while (br->count <= 56 && br->p < br->end) {
        br->bits |= (uint64_t)*br->p++ << br->count;
        br->count += 8;
    }
}

/* Next n (<= 32) bits without consuming them; zero-filled past the end. */
static inline uint32_t brl_peek(bitreader_lsb *br, unsigned n)
{
    if (br->count < n)
        brl_refill(br);
    return (uint32_t)(br->bits & ((1ull << n) - 1));
}

static inline void brl_consume(bitreader_lsb *br, unsigned n)
{
    if (n > br->count) {
        br->overrun = 1;
        br->bits = 0;
        br->count = 0;
        return;
    }
    br->bits >>= n;
    br->count -= n;
}

static inline uint32_t brl_read(bitreader_lsb *br, unsigned n)
{
    uint32_t v = brl_peek(br, n);
    brl_consume(br, n);
    return v;
}

/* Skip to the next byte boundary (DEFLATE stored blocks, LZW padding). */
static inline void brl_align(bitreader_lsb *br) { brl_consume(br, br->count % 8); }

/* Byte offset of the next unread byte; only meaningful when aligned. */
static inline size_t brl_tell(const bitreader_lsb *br, const uint8_t *start)
{
    return (size_t)(br->p - start) - br->count / 8;
}

/* ---- MSB-first reader ------------------------------------------------ */

static inline void brm_refill(bitreader_msb *br)
{
    while (br->count <= 56 && br->p < br->end) {
        br->bits |= (uint64_t)*br->p++ << (56 - br->count);
        br->count += 8;
    }
}

static inline uint32_t brm_peek(bitreader_msb *br, unsigned n)
{
    if (br->count < n)
        brm_refill(br);
    return n ? (uint32_t)(br->bits >> (64 - n)) : 0;
}

static inline void brm_consume(bitreader_msb *br, unsigned n)
{
    if (n > br->count) {
        br->overrun = 1;
        br->bits = 0;
        br->count = 0;
        return;
    }
    br->bits = n < 64 ? br->bits << n : 0;
    br->count -= n;
}

static inline uint32_t brm_read(bitreader_msb *br, unsigned n)
{
    uint32_t v = brm_peek(br, n);
    brm_consume(br, n);
    return v;
}

static inline void brm_align(bitreader_msb *br) { brm_consume(br, br->count % 8); }

static inline size_t brm_tell(const bitreader_msb *br, const uint8_t *start)
{
    return (size_t)(br->p - start) - br->count / 8;
}

/* ---- writers --------------------------------------------------------- */

/* Append the low n (<= 32) bits of v. */
static inline void bwl_put(bitwriter_lsb *bw, uint32_t v, unsigned n)
{
    bw->bits |= (uint64_t)(v & ((1ull << n) - 1)) << bw->count;
    bw->count += n;
    while (bw->count >= 8) {
        if (sq_buf_push(bw->out, (uint8_t)bw->bits) && !bw->err)
            bw->err = SQ_ERR_MEM;
        bw->bits >>= 8;
        bw->count -= 8;
    }
}

static inline void bwm_put(bitwriter_msb *bw, uint32_t v, unsigned n)
{
    if (n == 0)
        return;
    bw->bits |= (uint64_t)(v & ((1ull << n) - 1)) << (64 - bw->count - n);
    bw->count += n;
    while (bw->count >= 8) {
        if (sq_buf_push(bw->out, (uint8_t)(bw->bits >> 56)) && !bw->err)
            bw->err = SQ_ERR_MEM;
        bw->bits <<= 8;
        bw->count -= 8;
    }
}

#endif
