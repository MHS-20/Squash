#include <stdlib.h>

#include "lzma.h"

int lzma_props_decode(uint8_t props, unsigned *lc, unsigned *lp, unsigned *pb)
{
    if (props >= 9 * 5 * 5)
        return SQ_ERR_DATA;
    *lc = props % 9;
    props /= 9;
    *lp = props % 5;
    *pb = props / 5;
    return SQ_OK;
}

int lzma_model_init(lzma_model *m, unsigned lc, unsigned lp, unsigned pb)
{
    if (lc > 8 || lp > 4 || pb > 4)
        return SQ_ERR_DATA;
    m->lc = lc;
    m->lp = lp;
    m->pb = pb;
    m->literal = malloc(sizeof(rc_prob) * ((size_t)LZMA_LIT_PROBS << (lc + lp)));
    if (!m->literal)
        return SQ_ERR_MEM;
    lzma_model_reset(m);
    return SQ_OK;
}

void lzma_model_free(lzma_model *m)
{
    free(m->literal);
    m->literal = NULL;
}

static void reset_len(lzma_len_model *l)
{
    l->choice = l->choice2 = RC_PROB_INIT;
    rc_probs_init(&l->low[0][0], sizeof l->low / sizeof(rc_prob));
    rc_probs_init(&l->mid[0][0], sizeof l->mid / sizeof(rc_prob));
    rc_probs_init(l->high, sizeof l->high / sizeof(rc_prob));
}

void lzma_model_reset(lzma_model *m)
{
    rc_probs_init(&m->is_match[0][0], sizeof m->is_match / sizeof(rc_prob));
    rc_probs_init(m->is_rep, LZMA_NUM_STATES);
    rc_probs_init(m->is_rep_g0, LZMA_NUM_STATES);
    rc_probs_init(m->is_rep_g1, LZMA_NUM_STATES);
    rc_probs_init(m->is_rep_g2, LZMA_NUM_STATES);
    rc_probs_init(&m->is_rep0_long[0][0], sizeof m->is_rep0_long / sizeof(rc_prob));
    rc_probs_init(&m->dist_slot[0][0], sizeof m->dist_slot / sizeof(rc_prob));
    rc_probs_init(m->dist_special, sizeof m->dist_special / sizeof(rc_prob));
    rc_probs_init(m->dist_align, sizeof m->dist_align / sizeof(rc_prob));
    reset_len(&m->len);
    reset_len(&m->rep_len);
    rc_probs_init(m->literal, (size_t)LZMA_LIT_PROBS << (m->lc + m->lp));
    m->state = 0;
    for (int i = 0; i < LZMA_NUM_REPS; i++)
        m->rep[i] = 0;
}
