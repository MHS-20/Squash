/* Binary range coder with adaptive probabilities, as used by LZMA.
 *
 * Every decision is one bit coded with an 11-bit probability that the bit
 * is 0. After each bit the probability moves 1/32 of the way toward what
 * was seen, so the model adapts without counting. The coder state is a
 * 32-bit range; bytes leave the encoder once the range drops below 2^24,
 * with carries resolved through a one-byte cache plus a count of pending
 * 0xFF bytes. */
#ifndef SQ_RANGECODER_H
#define SQ_RANGECODER_H

#include "squash.h"

#define RC_PROB_BITS 11
#define RC_PROB_INIT (1u << (RC_PROB_BITS - 1))  /* 0.5 */
#define RC_MOVE_BITS 5
#define RC_TOP (1u << 24)

typedef uint16_t rc_prob;

typedef struct {
    uint64_t low;        /* 33 bits: bit 32 is a pending carry */
    uint32_t range;
    uint8_t cache;       /* last byte not yet written: a carry may change it */
    uint64_t cache_size; /* that byte plus the 0xFF bytes queued behind it */
    sq_buf *out;
    int err;
} rc_encoder;

typedef struct {
    const uint8_t *p, *end;
    uint32_t range, code;
    int overrun;         /* read past the end of input */
} rc_decoder;

void rc_probs_init(rc_prob *p, size_t n);

void rc_enc_init(rc_encoder *e, sq_buf *out);
void rc_enc_bit(rc_encoder *e, rc_prob *p, unsigned bit);
void rc_enc_direct(rc_encoder *e, uint32_t value, unsigned nbits);  /* p = 1/2, no model */
int rc_enc_flush(rc_encoder *e);
/* `nbits`-bit value through a binary tree of 2^nbits probabilities, MSB
 * first: each bit's probability depends on the bits before it. */
void rc_enc_tree(rc_encoder *e, rc_prob *probs, unsigned nbits, uint32_t value);
/* The same tree walked LSB first (LZMA's low distance bits). */
void rc_enc_tree_rev(rc_encoder *e, rc_prob *probs, unsigned nbits, uint32_t value);

/* SQ_ERR_DATA if the stream does not start with the mandatory zero byte. */
int rc_dec_init(rc_decoder *d, const uint8_t *p, size_t n);
unsigned rc_dec_bit(rc_decoder *d, rc_prob *p);
uint32_t rc_dec_direct(rc_decoder *d, unsigned nbits);
uint32_t rc_dec_tree(rc_decoder *d, rc_prob *probs, unsigned nbits);
uint32_t rc_dec_tree_rev(rc_decoder *d, rc_prob *probs, unsigned nbits);
/* Bytes consumed so far. */
static inline size_t rc_dec_pos(const rc_decoder *d, const uint8_t *start) { return (size_t)(d->p - start); }

/* Cost of coding `bit` with probability p, in 1/16 bits (for encoders). */
uint32_t rc_bit_price(rc_prob p, unsigned bit);

#endif
