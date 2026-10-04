/* LZMA decoder. Each step first decodes "is this a match?" in the context
 * of the state and position; a match then says whether it reuses one of
 * the four most recent distances ("rep") and which, or brings a new one. */
#include "lzma.h"

static uint32_t decode_len(rc_decoder *rc, lzma_len_model *l, unsigned pos_state)
{
    if (!rc_dec_bit(rc, &l->choice))
        return rc_dec_tree(rc, l->low[pos_state], 3);
    if (!rc_dec_bit(rc, &l->choice2))
        return 8 + rc_dec_tree(rc, l->mid[pos_state], 3);
    return 16 + rc_dec_tree(rc, l->high, 8);
}

/* Distances are sent as a 6-bit slot (roughly the bit length, plus the
 * next bit) followed by the remaining low bits: modelled for small
 * distances, raw in the middle and modelled again for the lowest 4 bits
 * of large distances, which are often aligned. */
static uint32_t decode_dist(rc_decoder *rc, lzma_model *m, uint32_t len)
{
    unsigned len_state = len < LZMA_LEN_STATES ? len : LZMA_LEN_STATES - 1;
    uint32_t slot = rc_dec_tree(rc, m->dist_slot[len_state], LZMA_DIST_SLOT_BITS);
    if (slot < 4)
        return slot;
    unsigned nbits = (slot >> 1) - 1;
    uint32_t dist = (2 | (slot & 1)) << nbits;
    if (slot < LZMA_END_POS_MODEL)
        return dist + rc_dec_tree_rev(rc, m->dist_special + dist - slot, nbits);
    dist += rc_dec_direct(rc, nbits - LZMA_ALIGN_BITS) << LZMA_ALIGN_BITS;
    return dist + rc_dec_tree_rev(rc, m->dist_align, LZMA_ALIGN_BITS);
}

static int decode_literal(rc_decoder *rc, lzma_model *m, sq_buf *out, size_t dict_start)
{
    size_t pos = out->len - dict_start;
    uint8_t prev = pos ? out->data[out->len - 1] : 0;
    rc_prob *probs = lzma_literal_probs(m, pos, prev);
    uint32_t sym = 1;
    if (m->state >= LZMA_NUM_LIT_STATES) {
        /* Right after a match, the byte at rep0 is a strong hint: code
         * bits in the context of the corresponding bit of that byte until
         * the first disagreement. */
        if ((size_t)m->rep[0] + 1 > pos)
            return SQ_ERR_DATA;
        unsigned match_byte = out->data[out->len - m->rep[0] - 1];
        do {
            unsigned match_bit = (match_byte >> 7) & 1;
            match_byte <<= 1;
            unsigned bit = rc_dec_bit(rc, &probs[((1 + match_bit) << 8) + sym]);
            sym = sym << 1 | bit;
            if (bit != match_bit)
                break;
        } while (sym < 0x100);
    }
    while (sym < 0x100)
        sym = sym << 1 | rc_dec_bit(rc, &probs[sym]);
    m->state = lzma_after_literal(m->state);
    return sq_buf_push(out, (uint8_t)sym);
}

int lzma_decode(lzma_model *m, rc_decoder *rc, sq_buf *out, size_t dict_start, size_t target,
                int allow_end, int *ended)
{
    unsigned pb_mask = (1u << m->pb) - 1;
    *ended = 0;
    while (out->len < target) {
        if (rc->overrun)
            return SQ_ERR_DATA;
        size_t pos = out->len - dict_start;
        unsigned ps = pos & pb_mask;
        int err;
        if (!rc_dec_bit(rc, &m->is_match[m->state][ps])) {
            err = decode_literal(rc, m, out, dict_start);
            if (err)
                return err;
            continue;
        }
        uint32_t len;
        if (rc_dec_bit(rc, &m->is_rep[m->state])) {
            if (pos == 0)
                return SQ_ERR_DATA;  /* nothing to repeat yet */
            if (!rc_dec_bit(rc, &m->is_rep_g0[m->state])) {
                if (!rc_dec_bit(rc, &m->is_rep0_long[m->state][ps])) {
                    /* "Short rep": one byte from distance rep0. */
                    m->state = lzma_after_shortrep(m->state);
                    err = sq_buf_lzcopy(out, dict_start, (size_t)m->rep[0] + 1, 1);
                    if (err)
                        return err;
                    continue;
                }
            } else {
                /* Move the chosen distance to the front of the list. */
                uint32_t dist;
                if (!rc_dec_bit(rc, &m->is_rep_g1[m->state])) {
                    dist = m->rep[1];
                } else {
                    if (!rc_dec_bit(rc, &m->is_rep_g2[m->state])) {
                        dist = m->rep[2];
                    } else {
                        dist = m->rep[3];
                        m->rep[3] = m->rep[2];
                    }
                    m->rep[2] = m->rep[1];
                }
                m->rep[1] = m->rep[0];
                m->rep[0] = dist;
            }
            len = decode_len(rc, &m->rep_len, ps);
            m->state = lzma_after_rep(m->state);
        } else {
            m->rep[3] = m->rep[2];
            m->rep[2] = m->rep[1];
            m->rep[1] = m->rep[0];
            len = decode_len(rc, &m->len, ps);
            m->state = lzma_after_match(m->state);
            m->rep[0] = decode_dist(rc, m, len);
            if (m->rep[0] == LZMA_END_MARKER) {
                if (!allow_end || rc->overrun)
                    return SQ_ERR_DATA;
                *ended = 1;
                return SQ_OK;
            }
        }
        len += LZMA_MIN_MATCH;
        if (rc->overrun || len > target - out->len)
            return SQ_ERR_DATA;  /* a match may not run past the end */
        err = sq_buf_lzcopy(out, dict_start, (size_t)m->rep[0] + 1, len);
        if (err)
            return err;
    }
    return rc->overrun ? SQ_ERR_DATA : SQ_OK;
}
