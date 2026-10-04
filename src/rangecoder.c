#include "rangecoder.h"

#include <math.h>
#include <threads.h>

void rc_probs_init(rc_prob *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = RC_PROB_INIT;
}

/* ---- encoder --------------------------------------------------------- */

void rc_enc_init(rc_encoder *e, sq_buf *out)
{
    e->low = 0;
    e->range = 0xFFFFFFFFu;
    e->cache = 0;
    e->cache_size = 1;  /* the stream therefore starts with a zero byte */
    e->out = out;
    e->err = SQ_OK;
}

static void put(rc_encoder *e, uint8_t b)
{
    if (sq_buf_push(e->out, b) && !e->err)
        e->err = SQ_ERR_MEM;
}

/* Moves the top byte of `low` out. A byte can only be written once it is
 * known no carry will reach it: if the byte is 0xFF a later carry would
 * ripple through it, so it is counted instead of written. */
static void shift_low(rc_encoder *e)
{
    if ((uint32_t)e->low < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        put(e, (uint8_t)(e->cache + carry));
        for (; e->cache_size > 1; e->cache_size--)
            put(e, (uint8_t)(0xFF + carry));
        e->cache_size = 0;
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    e->low = (e->low & 0x00FFFFFFu) << 8;
}

static void normalize_enc(rc_encoder *e)
{
    while (e->range < RC_TOP) {
        e->range <<= 8;
        shift_low(e);
    }
}

void rc_enc_bit(rc_encoder *e, rc_prob *p, unsigned bit)
{
    uint32_t bound = (e->range >> RC_PROB_BITS) * *p;
    if (bit == 0) {
        e->range = bound;
        *p += ((1u << RC_PROB_BITS) - *p) >> RC_MOVE_BITS;
    } else {
        e->low += bound;
        e->range -= bound;
        *p -= *p >> RC_MOVE_BITS;
    }
    normalize_enc(e);
}

void rc_enc_direct(rc_encoder *e, uint32_t value, unsigned nbits)
{
    while (nbits--) {
        e->range >>= 1;
        if ((value >> nbits) & 1)
            e->low += e->range;
        normalize_enc(e);
    }
}

int rc_enc_flush(rc_encoder *e)
{
    /* Push all of `low` (plus the cache) out. */
    for (int i = 0; i < 5; i++)
        shift_low(e);
    return e->err;
}

void rc_enc_tree(rc_encoder *e, rc_prob *probs, unsigned nbits, uint32_t value)
{
    uint32_t m = 1;  /* node index: 1, then 2-3, then 4-7, ... */
    while (nbits--) {
        unsigned bit = (value >> nbits) & 1;
        rc_enc_bit(e, &probs[m], bit);
        m = m << 1 | bit;
    }
}

void rc_enc_tree_rev(rc_encoder *e, rc_prob *probs, unsigned nbits, uint32_t value)
{
    uint32_t m = 1;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned bit = (value >> i) & 1;
        rc_enc_bit(e, &probs[m], bit);
        m = m << 1 | bit;
    }
}

/* ---- decoder --------------------------------------------------------- */

static uint8_t next_byte(rc_decoder *d)
{
    if (d->p == d->end) {
        d->overrun = 1;
        return 0;
    }
    return *d->p++;
}

int rc_dec_init(rc_decoder *d, const uint8_t *p, size_t n)
{
    d->p = p;
    d->end = p + n;
    d->overrun = 0;
    d->range = 0xFFFFFFFFu;
    d->code = 0;
    if (next_byte(d) != 0)
        return SQ_ERR_DATA;
    for (int i = 0; i < 4; i++)
        d->code = d->code << 8 | next_byte(d);
    /* code must lie inside the range; 0xFFFFFFFF never can. */
    return d->overrun || d->code == 0xFFFFFFFFu ? SQ_ERR_DATA : SQ_OK;
}

static void normalize_dec(rc_decoder *d)
{
    if (d->range < RC_TOP) {
        d->range <<= 8;
        d->code = d->code << 8 | next_byte(d);
    }
}

unsigned rc_dec_bit(rc_decoder *d, rc_prob *p)
{
    uint32_t bound = (d->range >> RC_PROB_BITS) * *p;
    unsigned bit;
    if (d->code < bound) {
        d->range = bound;
        *p += ((1u << RC_PROB_BITS) - *p) >> RC_MOVE_BITS;
        bit = 0;
    } else {
        d->code -= bound;
        d->range -= bound;
        *p -= *p >> RC_MOVE_BITS;
        bit = 1;
    }
    normalize_dec(d);
    return bit;
}

uint32_t rc_dec_direct(rc_decoder *d, unsigned nbits)
{
    uint32_t v = 0;
    while (nbits--) {
        d->range >>= 1;
        unsigned bit = d->code >= d->range;
        if (bit)
            d->code -= d->range;
        v = v << 1 | bit;
        normalize_dec(d);
    }
    return v;
}

uint32_t rc_dec_tree(rc_decoder *d, rc_prob *probs, unsigned nbits)
{
    uint32_t m = 1;
    for (unsigned i = 0; i < nbits; i++)
        m = m << 1 | rc_dec_bit(d, &probs[m]);
    return m - (1u << nbits);
}

uint32_t rc_dec_tree_rev(rc_decoder *d, rc_prob *probs, unsigned nbits)
{
    uint32_t m = 1, v = 0;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned bit = rc_dec_bit(d, &probs[m]);
        m = m << 1 | bit;
        v |= bit << i;
    }
    return v;
}

/* ---- prices ---------------------------------------------------------- */

/* -log2(probability) in 1/16 bit units, sampled every 16 probability
 * steps: encoders price many bits per byte, so a table beats log2(). */
#define PRICE_SHIFT 4
static uint32_t price_table[(1u << RC_PROB_BITS) >> PRICE_SHIFT];
static once_flag price_once = ONCE_FLAG_INIT;

static void build_prices(void)
{
    for (uint32_t i = 0; i < (1u << RC_PROB_BITS) >> PRICE_SHIFT; i++) {
        double prob = (i * (1u << PRICE_SHIFT) + (1u << PRICE_SHIFT) / 2.0) / (1u << RC_PROB_BITS);
        price_table[i] = (uint32_t)(-log2(prob) * 16 + 0.5);
    }
}

uint32_t rc_bit_price(rc_prob p, unsigned bit)
{
    call_once(&price_once, build_prices);
    uint32_t q = bit ? (1u << RC_PROB_BITS) - p : p;
    return price_table[q >> PRICE_SHIFT];
}
