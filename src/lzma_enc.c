/* LZMA encoder: coding primitives for each kind of decision, and the fast
 * parser — greedy with one step of lazy evaluation and repeat-distance
 * preferences, the strategy of the LZMA SDK's "fast" mode. Levels 7-9 hand
 * the choice of decisions to the optimal parser in lzma_opt.c instead. */
#include <stdlib.h>

#include "lzma.h"

/* Default properties: 3 bits of previous-byte context for literals, no
 * literal position bits, 2 bits of position for match decisions. */
#define ENC_LC 3
#define ENC_LP 0
#define ENC_PB 2

static const struct {
    uint8_t dict_log, optimal;
    uint16_t chain, nice;
} levels[10] = {
    {0, 0, 0, 0},
    {20, 0, 4, 16}, {21, 0, 8, 32}, {22, 0, 16, 48}, {22, 0, 24, 64}, {23, 0, 32, 96},
    {23, 0, 64, 128}, {24, 1, 16, 32}, {24, 1, 32, 64}, {24, 1, 256, 128},
};

/* ---- coding primitives ----------------------------------------------- */

static void encode_len(rc_encoder *rc, lzma_len_model *l, uint32_t len, unsigned ps)
{
    if (len < 8) {
        rc_enc_bit(rc, &l->choice, 0);
        rc_enc_tree(rc, l->low[ps], 3, len);
    } else if (len < 16) {
        rc_enc_bit(rc, &l->choice, 1);
        rc_enc_bit(rc, &l->choice2, 0);
        rc_enc_tree(rc, l->mid[ps], 3, len - 8);
    } else {
        rc_enc_bit(rc, &l->choice, 1);
        rc_enc_bit(rc, &l->choice2, 1);
        rc_enc_tree(rc, l->high, 8, len - 16);
    }
}

static void encode_dist(rc_encoder *rc, lzma_model *m, uint32_t dist, uint32_t len)
{
    unsigned len_state = len < LZMA_LEN_STATES ? len : LZMA_LEN_STATES - 1;
    unsigned slot = lzma_dist_slot(dist);
    rc_enc_tree(rc, m->dist_slot[len_state], LZMA_DIST_SLOT_BITS, slot);
    if (slot < 4)
        return;
    unsigned nbits = (slot >> 1) - 1;
    uint32_t base = (2 | (slot & 1)) << nbits, low = dist - base;
    if (slot < LZMA_END_POS_MODEL) {
        rc_enc_tree_rev(rc, m->dist_special + base - slot, nbits, low);
    } else {
        rc_enc_direct(rc, low >> LZMA_ALIGN_BITS, nbits - LZMA_ALIGN_BITS);
        rc_enc_tree_rev(rc, m->dist_align, LZMA_ALIGN_BITS, low & ((1u << LZMA_ALIGN_BITS) - 1));
    }
}

static unsigned pos_state(const lzma_encoder *e) { return e->pos & ((1u << e->m.pb) - 1); }

static rc_prob *literal_probs(const lzma_encoder *e)
{
    return lzma_literal_probs(&e->m, e->pos, e->pos ? e->in[e->pos - 1] : 0);
}

static void encode_literal(lzma_encoder *e, rc_encoder *rc)
{
    lzma_model *m = &e->m;
    rc_enc_bit(rc, &m->is_match[m->state][pos_state(e)], 0);
    rc_prob *probs = literal_probs(e);
    unsigned byte = e->in[e->pos], sym = 1;
    if (m->state >= LZMA_NUM_LIT_STATES) {
        unsigned match_byte = e->in[e->pos - m->rep[0] - 1];
        int matched = 1;
        for (int i = 7; i >= 0; i--) {
            unsigned bit = (byte >> i) & 1;
            if (matched) {
                unsigned match_bit = (match_byte >> i) & 1;
                rc_enc_bit(rc, &probs[((1 + match_bit) << 8) + sym], bit);
                matched = bit == match_bit;
            } else {
                rc_enc_bit(rc, &probs[sym], bit);
            }
            sym = sym << 1 | bit;
        }
    } else {
        rc_enc_tree(rc, probs, 8, byte);
    }
    m->state = lzma_after_literal(m->state);
}

static void encode_shortrep(lzma_encoder *e, rc_encoder *rc)
{
    lzma_model *m = &e->m;
    unsigned ps = pos_state(e);
    rc_enc_bit(rc, &m->is_match[m->state][ps], 1);
    rc_enc_bit(rc, &m->is_rep[m->state], 1);
    rc_enc_bit(rc, &m->is_rep_g0[m->state], 0);
    rc_enc_bit(rc, &m->is_rep0_long[m->state][ps], 0);
    m->state = lzma_after_shortrep(m->state);
}

static void encode_rep(lzma_encoder *e, rc_encoder *rc, unsigned idx, uint32_t len)
{
    lzma_model *m = &e->m;
    unsigned ps = pos_state(e);
    rc_enc_bit(rc, &m->is_match[m->state][ps], 1);
    rc_enc_bit(rc, &m->is_rep[m->state], 1);
    if (idx == 0) {
        rc_enc_bit(rc, &m->is_rep_g0[m->state], 0);
        rc_enc_bit(rc, &m->is_rep0_long[m->state][ps], 1);
    } else {
        rc_enc_bit(rc, &m->is_rep_g0[m->state], 1);
        rc_enc_bit(rc, &m->is_rep_g1[m->state], idx != 1);
        if (idx != 1)
            rc_enc_bit(rc, &m->is_rep_g2[m->state], idx != 2);
        uint32_t dist = m->rep[idx];
        for (unsigned i = idx; i > 0; i--)
            m->rep[i] = m->rep[i - 1];
        m->rep[0] = dist;
    }
    encode_len(rc, &m->rep_len, len - LZMA_MIN_MATCH, ps);
    m->state = lzma_after_rep(m->state);
}

/* `dist` is distance - 1, as LZMA stores it. */
static void encode_match(lzma_encoder *e, rc_encoder *rc, uint32_t dist, uint32_t len)
{
    lzma_model *m = &e->m;
    unsigned ps = pos_state(e);
    rc_enc_bit(rc, &m->is_match[m->state][ps], 1);
    rc_enc_bit(rc, &m->is_rep[m->state], 0);
    encode_len(rc, &m->len, len - LZMA_MIN_MATCH, ps);
    encode_dist(rc, m, dist, len - LZMA_MIN_MATCH);
    m->rep[3] = m->rep[2];
    m->rep[2] = m->rep[1];
    m->rep[1] = m->rep[0];
    m->rep[0] = dist;
    m->state = lzma_after_match(m->state);
}

/* ---- parsing --------------------------------------------------------- */

int lzma_encoder_init(lzma_encoder *e, const uint8_t *in, size_t n, int level)
{
    if (level == 0)
        level = 6;
    if (level < 1 || level > 9)
        return SQ_ERR_ARG;
    /* A dictionary larger than the input buys nothing but memory. */
    unsigned dict_log = levels[level].dict_log;
    while (dict_log > 12 && ((size_t)1 << (dict_log - 1)) >= n)
        dict_log--;
    *e = (lzma_encoder){.in = in, .n = n, .cached_pos = SIZE_MAX, .nice_len = levels[level].nice,
                        .optimal = levels[level].optimal};
    e->params = (lz_params){
        .window = 1u << dict_log, .max_dist = (1u << dict_log) - 1,
        .min_match = 3, .max_match = LZMA_MAX_MATCH,
        .max_chain = levels[level].chain, .nice_len = levels[level].nice,
        .hash_bits = dict_log < 20 ? dict_log : 20,
    };
    int err = lzma_model_init(&e->m, ENC_LC, ENC_LP, ENC_PB);
    if (!err && (err = lz_matcher_init(&e->finder, in, n, &e->params)))
        lzma_model_free(&e->m);
    return err;
}

void lzma_encoder_free(lzma_encoder *e)
{
    lzma_model_free(&e->m);
    lz_matcher_free(&e->finder);
    lzma_opt_free(e);
}

uint32_t lzma_encoder_dict_size(const lzma_encoder *e) { return e->params.window; }

void lzma_encoder_reset_state(lzma_encoder *e)
{
    lzma_model_reset(&e->m);
    e->plan_len = e->plan_pos = 0;
}

void lzma_encoder_index_upto(lzma_encoder *e, size_t end)
{
    for (; e->indexed < end; e->indexed++)
        lz_matcher_insert(&e->finder, e->indexed);
}

/* Big distances cost more bits, so a slightly shorter repeat (whose
 * distance is free) wins over a new match. Thresholds from the LZMA SDK's
 * fast mode. */
static int rep_beats_match(uint32_t rep_len, lz_match main)
{
    return rep_len >= 2 && (rep_len + 1 >= main.len ||
                            (rep_len + 2 >= main.len && main.dist >= (1u << 9)) ||
                            (rep_len + 3 >= main.len && main.dist >= (1u << 15)));
}

static lzma_op fast_next(lzma_encoder *e)
{
    size_t pos = e->pos, avail = e->n - pos;
    uint32_t max = avail < LZMA_MAX_MATCH ? (uint32_t)avail : LZMA_MAX_MATCH;
    lzma_encoder_index_upto(e, pos);
    lz_match main = e->cached_pos == pos ? e->cached : lz_matcher_find(&e->finder, pos);
    lzma_encoder_index_upto(e, pos + 1);

    unsigned rep_idx = 0;
    uint32_t rep_len = 0;
    for (unsigned r = 0; r < LZMA_NUM_REPS; r++) {
        size_t d = (size_t)e->m.rep[r] + 1;
        if (d > pos)
            continue;
        const uint8_t *a = e->in + pos, *b = a - d;
        uint32_t len = 0;
        while (len < max && a[len] == b[len])
            len++;
        if (len > rep_len)
            rep_len = len, rep_idx = r;
    }

    if (rep_len >= e->nice_len || (rep_len >= 2 && rep_beats_match(rep_len, main)))
        return (lzma_op){LZMA_OP_REP, (uint8_t)rep_idx, rep_len, 0};
    if (main.len >= LZMA_MIN_MATCH) {
        /* Lazy step: if the next position matches longer, emit a literal
         * now and take that match next time. */
        if (main.len < e->nice_len && pos + 1 < e->n) {
            e->cached = lz_matcher_find(&e->finder, pos + 1);
            e->cached_pos = pos + 1;
        }
        if (e->cached_pos != pos + 1 || e->cached.len <= main.len) {
            uint32_t dist = main.dist - 1;
            for (unsigned r = 0; r < LZMA_NUM_REPS; r++)
                if (e->m.rep[r] == dist)  /* same distance as a recent one */
                    return (lzma_op){LZMA_OP_REP, (uint8_t)r, main.len, 0};
            return (lzma_op){LZMA_OP_MATCH, 0, main.len, dist};
        }
    }
    const lzma_model *m = &e->m;
    unsigned ps = pos & ((1u << m->pb) - 1);
    if (pos > m->rep[0] && e->in[pos] == e->in[pos - m->rep[0] - 1] &&
        lzma_price_shortrep(m, m->state, ps) < lzma_price_literal(m, e->in, pos, m->state, m->rep[0]))
        return (lzma_op){LZMA_OP_SHORTREP, 0, 1, 0};
    return (lzma_op){LZMA_OP_LIT, 0, 1, 0};
}

uint32_t lzma_encode_step(lzma_encoder *e, rc_encoder *rc)
{
    lzma_op op = e->optimal ? lzma_opt_next(e) : fast_next(e);
    switch (op.kind) {
    case LZMA_OP_LIT: encode_literal(e, rc); break;
    case LZMA_OP_SHORTREP: encode_shortrep(e, rc); break;
    case LZMA_OP_REP: encode_rep(e, rc, op.rep, op.len); break;
    default: encode_match(e, rc, op.dist, op.len); break;
    }
    e->pos += op.len;
    return op.len;
}

void lzma_encode_end_marker(lzma_encoder *e, rc_encoder *rc)
{
    lzma_model *m = &e->m;
    unsigned ps = pos_state(e);
    rc_enc_bit(rc, &m->is_match[m->state][ps], 1);
    rc_enc_bit(rc, &m->is_rep[m->state], 0);
    encode_len(rc, &m->len, 0, ps);
    encode_dist(rc, m, LZMA_END_MARKER, 0);
}
