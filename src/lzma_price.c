/* Bit-cost estimates for LZMA decisions, read off the current model: a
 * decision costs the sum of -log2 p over the bits that code it. */
#include "lzma.h"

unsigned lzma_dist_slot(uint32_t dist0)
{
    if (dist0 < 4)
        return dist0;
    /* Slot = 2 * floor(log2(dist)) + the bit below the top one. */
    unsigned top = 31 - (unsigned)__builtin_clz(dist0);
    return 2 * top + ((dist0 >> (top - 1)) & 1);
}

static uint32_t price_tree(const rc_prob *probs, unsigned nbits, uint32_t value)
{
    uint32_t price = 0, m = 1;
    while (nbits--) {
        unsigned bit = (value >> nbits) & 1;
        price += rc_bit_price(probs[m], bit);
        m = m << 1 | bit;
    }
    return price;
}

static uint32_t price_tree_rev(const rc_prob *probs, unsigned nbits, uint32_t value)
{
    uint32_t price = 0, m = 1;
    for (unsigned i = 0; i < nbits; i++) {
        unsigned bit = (value >> i) & 1;
        price += rc_bit_price(probs[m], bit);
        m = m << 1 | bit;
    }
    return price;
}

uint32_t lzma_price_literal(const lzma_model *m, const uint8_t *in, size_t pos, unsigned state,
                            uint32_t rep0)
{
    const rc_prob *probs = lzma_literal_probs(m, pos, pos ? in[pos - 1] : 0);
    unsigned byte = in[pos], sym = 1;
    uint32_t price = rc_bit_price(m->is_match[state][pos & ((1u << m->pb) - 1)], 0);
    int matched = state >= LZMA_NUM_LIT_STATES;
    unsigned match_byte = matched ? in[pos - rep0 - 1] : 0;
    for (int i = 7; i >= 0; i--) {
        unsigned bit = (byte >> i) & 1;
        if (matched) {
            unsigned match_bit = (match_byte >> i) & 1;
            price += rc_bit_price(probs[((1 + match_bit) << 8) + sym], bit);
            matched = bit == match_bit;
        } else {
            price += rc_bit_price(probs[sym], bit);
        }
        sym = sym << 1 | bit;
    }
    return price;
}

uint32_t lzma_price_shortrep(const lzma_model *m, unsigned state, unsigned ps)
{
    return rc_bit_price(m->is_match[state][ps], 1) + rc_bit_price(m->is_rep[state], 1) +
           rc_bit_price(m->is_rep_g0[state], 0) + rc_bit_price(m->is_rep0_long[state][ps], 0);
}

uint32_t lzma_price_rep(const lzma_model *m, unsigned idx, unsigned state, unsigned ps)
{
    uint32_t price = rc_bit_price(m->is_match[state][ps], 1) + rc_bit_price(m->is_rep[state], 1);
    if (idx == 0)
        return price + rc_bit_price(m->is_rep_g0[state], 0) +
               rc_bit_price(m->is_rep0_long[state][ps], 1);
    price += rc_bit_price(m->is_rep_g0[state], 1);
    if (idx == 1)
        return price + rc_bit_price(m->is_rep_g1[state], 0);
    return price + rc_bit_price(m->is_rep_g1[state], 1) + rc_bit_price(m->is_rep_g2[state], idx == 3);
}

uint32_t lzma_price_match(const lzma_model *m, unsigned state, unsigned ps)
{
    return rc_bit_price(m->is_match[state][ps], 1) + rc_bit_price(m->is_rep[state], 0);
}

uint32_t lzma_price_len(const lzma_len_model *l, uint32_t len0, unsigned ps)
{
    if (len0 < 8)
        return rc_bit_price(l->choice, 0) + price_tree(l->low[ps], 3, len0);
    if (len0 < 16)
        return rc_bit_price(l->choice, 1) + rc_bit_price(l->choice2, 0) +
               price_tree(l->mid[ps], 3, len0 - 8);
    return rc_bit_price(l->choice, 1) + rc_bit_price(l->choice2, 1) +
           price_tree(l->high, 8, len0 - 16);
}

uint32_t lzma_price_dist(const lzma_model *m, uint32_t dist0, uint32_t len0)
{
    unsigned len_state = len0 < LZMA_LEN_STATES ? len0 : LZMA_LEN_STATES - 1;
    unsigned slot = lzma_dist_slot(dist0);
    uint32_t price = price_tree(m->dist_slot[len_state], LZMA_DIST_SLOT_BITS, slot);
    if (slot < 4)
        return price;
    unsigned nbits = (slot >> 1) - 1;
    uint32_t base = (2 | (slot & 1)) << nbits, low = dist0 - base;
    if (slot < LZMA_END_POS_MODEL)
        return price + price_tree_rev(m->dist_special + base - slot, nbits, low);
    /* Direct bits cost exactly one bit each (16 in these units). */
    return price + 16 * (nbits - LZMA_ALIGN_BITS) +
           price_tree_rev(m->dist_align, LZMA_ALIGN_BITS, low & ((1u << LZMA_ALIGN_BITS) - 1));
}
