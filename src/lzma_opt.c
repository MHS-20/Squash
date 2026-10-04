/* LZMA optimal parsing: instead of deciding byte by byte, price every way
 * to cover the next stretch of input and code the cheapest path.
 *
 * Node i stands for "the first i bytes from here are coded". From each
 * reachable node every decision is tried — a literal, a short rep, each
 * repeat distance at every length, each match the finder reports at every
 * length — and the node it lands on keeps the cheapest way in. Because
 * LZMA's costs depend on the state and the repeat distances, each node
 * also records those as they would be after its best path. The stretch
 * ends where no decision reaches further, after OPT_SPAN bytes, or at a
 * match long enough to take outright; then the path is traced back and
 * queued.
 *
 * Prices come from the model as it is when the stretch starts and drift
 * slightly as coding updates it; the LZMA SDK refreshes them the same way. */
#include <stdlib.h>
#include <string.h>

#include "lzma.h"

#define OPT_SPAN 4096
#define OPT_NODES (OPT_SPAN + LZMA_MAX_MATCH + 1)
#define MAX_PAIRS 64
#define INFINITE_PRICE UINT32_MAX

/* Recomputing all length prices costs about as much as pricing a few
 * hundred bytes, so they are refreshed only this often. */
#define LEN_PRICE_REFRESH 1024

typedef struct {
    uint32_t price;
    uint32_t from;     /* node this one is reached from */
    lzma_op op;        /* the decision taken there */
    uint8_t state;
    uint32_t rep[LZMA_NUM_REPS];
} opt_node;

struct lzma_opt {
    opt_node nodes[OPT_NODES];
    lzma_op plan[OPT_NODES];
    uint32_t len[LZMA_POS_STATES_MAX][LZMA_MAX_MATCH + 1];      /* prices by length */
    uint32_t rep_len[LZMA_POS_STATES_MAX][LZMA_MAX_MATCH + 1];
    size_t priced_at;  /* input position of the last length-price refresh */
    int priced;
};

static void fill_len_prices(const lzma_model *m, lzma_opt *o)
{
    for (unsigned ps = 0; ps < (1u << m->pb); ps++)
        for (uint32_t l = LZMA_MIN_MATCH; l <= LZMA_MAX_MATCH; l++) {
            o->len[ps][l] = lzma_price_len(&m->len, l - LZMA_MIN_MATCH, ps);
            o->rep_len[ps][l] = lzma_price_len(&m->rep_len, l - LZMA_MIN_MATCH, ps);
        }
}

/* Makes nodes up to `upto` usable: unreached, at infinite price. */
static void reach(opt_node *nodes, uint32_t *ready, uint32_t upto)
{
    for (; *ready < upto; (*ready)++)
        nodes[*ready + 1].price = INFINITE_PRICE;
}

/* Moves to node `to` if this way in is cheaper, recording the state and
 * repeat distances after the decision. */
static void relax(opt_node *nodes, uint32_t from, uint32_t to, uint32_t price, lzma_op op)
{
    opt_node *t = &nodes[to];
    if (price >= t->price)
        return;
    const opt_node *f = &nodes[from];
    t->price = price;
    t->from = from;
    t->op = op;
    memcpy(t->rep, f->rep, sizeof t->rep);
    switch (op.kind) {
    case LZMA_OP_LIT:
        t->state = (uint8_t)lzma_after_literal(f->state);
        break;
    case LZMA_OP_SHORTREP:
        t->state = (uint8_t)lzma_after_shortrep(f->state);
        break;
    case LZMA_OP_REP:
        t->state = (uint8_t)lzma_after_rep(f->state);
        for (unsigned i = op.rep; i > 0; i--)
            t->rep[i] = f->rep[i - 1];
        t->rep[0] = f->rep[op.rep];
        break;
    default:
        t->state = (uint8_t)lzma_after_match(f->state);
        memmove(t->rep + 1, f->rep, 3 * sizeof(uint32_t));
        t->rep[0] = op.dist;
        break;
    }
}

static uint32_t common(const uint8_t *a, const uint8_t *b, uint32_t max)
{
    uint32_t len = 0;
    while (len < max && a[len] == b[len])
        len++;
    return len;
}

/* Fills e->plan with the decisions covering the next stretch. */
static int plan_stretch(lzma_encoder *e)
{
    lzma_opt *o = e->opt;
    const lzma_model *m = &e->m;
    opt_node *nodes = o->nodes;
    if (!o->priced || e->pos - o->priced_at >= LEN_PRICE_REFRESH) {
        fill_len_prices(m, o);
        o->priced_at = e->pos;
        o->priced = 1;
    }
    unsigned pb_mask = (1u << m->pb) - 1;

    nodes[0].price = 0;
    nodes[0].state = (uint8_t)m->state;
    memcpy(nodes[0].rep, m->rep, sizeof nodes[0].rep);
    uint32_t last = 0;   /* furthest node reached so far */
    uint32_t ready = 0;  /* nodes 1..ready are initialised */

    /* Node `last` is where the stretch ends, so it is not expanded: the
     * next stretch starts there, and its position must not be indexed
     * yet (a position found in the finder before it is searched would
     * match itself). */
    for (uint32_t i = 0; (i == 0 || i < last) && i < OPT_SPAN; i++) {
        size_t pos = e->pos + i;
        if (pos >= e->n)
            break;
        const opt_node *cur = &nodes[i];
        uint32_t max = e->n - pos < LZMA_MAX_MATCH ? (uint32_t)(e->n - pos) : LZMA_MAX_MATCH;
        unsigned ps = pos & pb_mask, st = cur->state;

        lzma_encoder_index_upto(e, pos);
        lz_match pairs[MAX_PAIRS];
        size_t np = lz_matcher_find_all(&e->finder, pos, pairs, MAX_PAIRS);
        lzma_encoder_index_upto(e, pos + 1);

        uint32_t rep_len[LZMA_NUM_REPS] = {0}, longest = np ? pairs[np - 1].len : 0;
        for (unsigned r = 0; r < LZMA_NUM_REPS; r++) {
            if ((size_t)cur->rep[r] + 1 <= pos)
                rep_len[r] = common(e->in + pos, e->in + pos - cur->rep[r] - 1, max);
            if (rep_len[r] > longest)
                longest = rep_len[r];
        }
        reach(nodes, &ready, i + (longest > 1 ? longest : 1));

        /* A long enough match is taken without weighing alternatives,
         * which keeps runs of repetitive data from costing quadratic time. */
        if (longest >= e->nice_len) {
            lzma_op op = {LZMA_OP_MATCH, 0, longest, 0};
            uint32_t price;
            unsigned r = 0;
            while (r < LZMA_NUM_REPS && rep_len[r] != longest)
                r++;
            if (r < LZMA_NUM_REPS) {
                op = (lzma_op){LZMA_OP_REP, (uint8_t)r, longest, 0};
                price = lzma_price_rep(m, r, st, ps) + o->rep_len[ps][longest];
            } else {
                op.dist = pairs[np - 1].dist - 1;
                price = lzma_price_match(m, st, ps) + o->len[ps][longest] +
                        lzma_price_dist(m, op.dist, longest - LZMA_MIN_MATCH);
            }
            if (i == 0) {
                o->plan[0] = op;
                e->plan_len = 1;
                e->plan_pos = 0;
                return SQ_OK;
            }
            relax(nodes, i, i + longest, cur->price + price, op);
            last = i + longest;
            break;
        }

        relax(nodes, i, i + 1, cur->price + lzma_price_literal(m, e->in, pos, st, cur->rep[0]),
              (lzma_op){LZMA_OP_LIT, 0, 1, 0});
        if (pos > cur->rep[0] && e->in[pos] == e->in[pos - cur->rep[0] - 1])
            relax(nodes, i, i + 1, cur->price + lzma_price_shortrep(m, st, ps),
                  (lzma_op){LZMA_OP_SHORTREP, 0, 1, 0});
        for (unsigned r = 0; r < LZMA_NUM_REPS; r++) {
            uint32_t base = cur->price + lzma_price_rep(m, r, st, ps);
            for (uint32_t l = LZMA_MIN_MATCH; l <= rep_len[r]; l++)
                relax(nodes, i, i + l, base + o->rep_len[ps][l],
                      (lzma_op){LZMA_OP_REP, (uint8_t)r, l, 0});
        }
        uint32_t base = cur->price + lzma_price_match(m, st, ps), l = LZMA_MIN_MATCH;
        for (size_t k = 0; k < np; k++) {
            uint32_t dist = pairs[k].dist - 1;
            /* The distance price depends on the length only up to 5. */
            uint32_t dprice[LZMA_LEN_STATES];
            for (uint32_t s = 0; s < LZMA_LEN_STATES; s++)
                dprice[s] = lzma_price_dist(m, dist, s);
            for (; l <= pairs[k].len; l++) {
                uint32_t len0 = l - LZMA_MIN_MATCH;
                uint32_t d = dprice[len0 < LZMA_LEN_STATES ? len0 : LZMA_LEN_STATES - 1];
                relax(nodes, i, i + l, base + o->len[ps][l] + d,
                      (lzma_op){LZMA_OP_MATCH, 0, l, dist});
            }
        }
        if (i + longest > last)
            last = i + longest;
        if (i + 1 > last)
            last = i + 1;
    }

    /* Trace the cheapest path back from the furthest node, then queue it
     * front to back. */
    size_t count = 0;
    for (uint32_t j = last; j > 0; j = nodes[j].from)
        count++;
    e->plan_len = count;
    e->plan_pos = 0;
    for (uint32_t j = last; j > 0; j = nodes[j].from)
        o->plan[--count] = nodes[j].op;
    return SQ_OK;
}

lzma_op lzma_opt_next(lzma_encoder *e)
{
    if (!e->opt && !(e->opt = calloc(1, sizeof *e->opt)))
        return (lzma_op){LZMA_OP_LIT, 0, 1, 0};  /* out of memory: still valid */
    if (e->plan_pos == e->plan_len)
        plan_stretch(e);
    return e->opt->plan[e->plan_pos++];
}

void lzma_opt_free(lzma_encoder *e)
{
    free(e->opt);
    e->opt = NULL;
}
