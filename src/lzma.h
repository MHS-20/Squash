/* LZMA: LZ77 whose every decision is coded bit by bit with the adaptive
 * range coder, in contexts chosen to make the bits predictable.
 *
 * This header holds the probability model shared by the encoder and the
 * decoder, and the raw LZMA codec used by the .lzma and LZMA2 (xz)
 * containers. */
#ifndef SQ_LZMA_H
#define SQ_LZMA_H

#include "lz77.h"
#include "rangecoder.h"
#include "squash.h"

#define LZMA_NUM_STATES 12
#define LZMA_NUM_LIT_STATES 7    /* states 0-6: the last thing coded was a literal */
#define LZMA_POS_STATES_MAX 16   /* pb <= 4 */
#define LZMA_NUM_REPS 4
#define LZMA_MIN_MATCH 2
#define LZMA_MAX_MATCH 273
#define LZMA_LEN_STATES 4        /* distance slot context: match length 2, 3, 4, 5+ */
#define LZMA_DIST_SLOT_BITS 6
#define LZMA_END_POS_MODEL 14    /* slots below this code low bits with models */
#define LZMA_FULL_DISTANCES 128
#define LZMA_ALIGN_BITS 4
#define LZMA_LIT_PROBS 0x300     /* per literal context: 3 trees of 256 */
#define LZMA_END_MARKER 0xFFFFFFFFu  /* distance-1 value that ends a stream */

/* Match length: 3 bits (lengths 2-9) and 3 bits (10-17) per position
 * state, or 8 bits (18-273) shared, selected by two choice bits. */
typedef struct {
    rc_prob choice, choice2;
    rc_prob low[LZMA_POS_STATES_MAX][1 << 3];
    rc_prob mid[LZMA_POS_STATES_MAX][1 << 3];
    rc_prob high[1 << 8];
} lzma_len_model;

typedef struct {
    unsigned lc, lp, pb;  /* literal context bits, literal / match position bits */
    rc_prob is_match[LZMA_NUM_STATES][LZMA_POS_STATES_MAX];
    rc_prob is_rep[LZMA_NUM_STATES];
    rc_prob is_rep_g0[LZMA_NUM_STATES];
    rc_prob is_rep_g1[LZMA_NUM_STATES];
    rc_prob is_rep_g2[LZMA_NUM_STATES];
    rc_prob is_rep0_long[LZMA_NUM_STATES][LZMA_POS_STATES_MAX];
    rc_prob dist_slot[LZMA_LEN_STATES][1 << LZMA_DIST_SLOT_BITS];
    rc_prob dist_special[1 + LZMA_FULL_DISTANCES - LZMA_END_POS_MODEL];
    rc_prob dist_align[1 << LZMA_ALIGN_BITS];
    lzma_len_model len, rep_len;
    rc_prob *literal;     /* LZMA_LIT_PROBS << (lc + lp) */
    unsigned state;       /* what the last few operations were */
    uint32_t rep[LZMA_NUM_REPS];  /* recent distances, stored as distance - 1 */
} lzma_model;

/* Allocates the literal probabilities for lc/lp/pb and resets everything. */
int lzma_model_init(lzma_model *m, unsigned lc, unsigned lp, unsigned pb);
void lzma_model_free(lzma_model *m);
/* All probabilities to 1/2, state 0, reps 0 (LZMA2 "state reset"). */
void lzma_model_reset(lzma_model *m);
/* The properties byte: (pb * 5 + lp) * 9 + lc. */
int lzma_props_decode(uint8_t props, unsigned *lc, unsigned *lp, unsigned *pb);

static inline unsigned lzma_after_literal(unsigned s) { return s < 4 ? 0 : s < 10 ? s - 3 : s - 6; }
static inline unsigned lzma_after_match(unsigned s) { return s < LZMA_NUM_LIT_STATES ? 7 : 10; }
static inline unsigned lzma_after_rep(unsigned s) { return s < LZMA_NUM_LIT_STATES ? 8 : 11; }
static inline unsigned lzma_after_shortrep(unsigned s) { return s < LZMA_NUM_LIT_STATES ? 9 : 11; }

/* Literal probabilities for the byte at dictionary position `pos` after
 * byte `prev`: the context is the low lp bits of the position and the top
 * lc bits of the previous byte. */
static inline rc_prob *lzma_literal_probs(const lzma_model *m, size_t pos, uint8_t prev)
{
    size_t ctx = ((pos & ((1u << m->lp) - 1)) << m->lc) + (prev >> (8 - m->lc));
    return m->literal + LZMA_LIT_PROBS * ctx;
}

/* ---- decoder (lzma_dec.c) ---- */

/* Decodes until out->len reaches `target` or an end marker is read.
 * `dict_start` is where the dictionary (and the position count) begins.
 * With `allow_end`, an end marker sets *ended; otherwise it is an error. */
int lzma_decode(lzma_model *m, rc_decoder *rc, sq_buf *out, size_t dict_start, size_t target,
                int allow_end, int *ended);

/* ---- prices (lzma_price.c) ---- */

/* Estimated cost, in 1/16 bit, of coding a decision with the model as it
 * stands. Encoders use them to compare alternatives. `len0` is a match
 * length minus LZMA_MIN_MATCH and `dist0` a distance minus one, as coded. */
unsigned lzma_dist_slot(uint32_t dist0);
uint32_t lzma_price_literal(const lzma_model *m, const uint8_t *in, size_t pos, unsigned state,
                            uint32_t rep0);
uint32_t lzma_price_shortrep(const lzma_model *m, unsigned state, unsigned pos_state);
uint32_t lzma_price_rep(const lzma_model *m, unsigned idx, unsigned state, unsigned pos_state);
uint32_t lzma_price_match(const lzma_model *m, unsigned state, unsigned pos_state);
uint32_t lzma_price_len(const lzma_len_model *l, uint32_t len0, unsigned pos_state);
uint32_t lzma_price_dist(const lzma_model *m, uint32_t dist0, uint32_t len0);

/* ---- encoder (lzma_enc.c, lzma_opt.c) ---- */

enum { LZMA_OP_LIT, LZMA_OP_SHORTREP, LZMA_OP_REP, LZMA_OP_MATCH };

/* One coding decision: a literal, a one-byte repeat of rep0, a repeat of
 * recent distance `rep`, or a match at distance `dist` + 1. */
typedef struct {
    uint8_t kind, rep;
    uint32_t len, dist;
} lzma_op;

typedef struct lzma_opt lzma_opt;  /* the optimal parser's working state */

typedef struct {
    lzma_model m;
    lz_params params;
    lz_matcher finder;
    const uint8_t *in;
    size_t n, pos;       /* next byte to encode */
    size_t indexed;      /* positions below this are in the match finder */
    uint32_t nice_len;
    size_t cached_pos;   /* the fast parser's look-ahead match, reused */
    lz_match cached;
    int optimal;         /* levels 7-9: price whole paths (lzma_opt.c) */
    lzma_opt *opt;
    size_t plan_len, plan_pos;  /* planned decisions; reset with the model */
} lzma_encoder;

/* Dictionary size and search effort for levels 1-9 (0 = default 6),
 * shrunk for small inputs. */
int lzma_encoder_init(lzma_encoder *e, const uint8_t *in, size_t n, int level);
void lzma_encoder_free(lzma_encoder *e);
uint32_t lzma_encoder_dict_size(const lzma_encoder *e);
/* Codes the next decision at e->pos and advances past it. Returns the
 * number of input bytes it covered (at most LZMA_MAX_MATCH). */
uint32_t lzma_encode_step(lzma_encoder *e, rc_encoder *rc);
/* The end-of-stream marker (a match with distance 2^32). */
void lzma_encode_end_marker(lzma_encoder *e, rc_encoder *rc);
/* Resets the model (LZMA2 state reset) and drops any planned decisions,
 * which were priced against the old model. */
void lzma_encoder_reset_state(lzma_encoder *e);
/* Indexes every position below `end` not yet in the match finder. */
void lzma_encoder_index_upto(lzma_encoder *e, size_t end);

/* The optimal parser's next decision, and its cleanup (lzma_opt.c). */
lzma_op lzma_opt_next(lzma_encoder *e);
void lzma_opt_free(lzma_encoder *e);

/* ---- LZMA2 (lzma2.c) ---- */

int lzma2_compress(const uint8_t *in, size_t n, int level, sq_buf *out, uint32_t *dict_size);
/* Decodes one LZMA2 stream; *used gets the bytes consumed. */
int lzma2_decompress(const uint8_t *in, size_t n, sq_buf *out, size_t *used);

#endif
