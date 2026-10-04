/* Finite State Entropy: table-based asymmetric numeral systems (tANS) in
 * the exact form zstd uses (RFC 8878 section 4.1).
 *
 * Symbol probabilities are quantised to counts over a table of 2^log
 * states. Each state decodes to one symbol; after emitting it, the decoder
 * reads a few bits to choose the next state. The encoder runs the same
 * machine backwards, so a stream is decoded in the reverse of the order it
 * was written (see bitreader_rev). */
#ifndef SQ_FSE_H
#define SQ_FSE_H

#include "bitio.h"
#include "squash.h"

#define FSE_MIN_LOG 5
#define FSE_MAX_LOG 12
#define FSE_MAX_SYMS 256

/* Normalised counts: entries sum to 2^log, where -1 marks a "less than
 * one" symbol that still gets one state (of probability 1/2^log). */
typedef int16_t fse_norm;

/* Quantise `count[0..nsyms)` (summing to `total`) to 2^log. Every present
 * symbol keeps a non-zero count; with `allow_low` the rarest become -1. */
int fse_normalize(const uint32_t *count, unsigned nsyms, size_t total, unsigned log,
                  int allow_low, fse_norm *norm);

/* The compact table description of RFC 8878 4.1.1. Reading returns the
 * bytes consumed (0 on error) and sets *nsyms (last symbol + 1) and *log. */
size_t fse_read_ncount(const uint8_t *in, size_t n, fse_norm *norm, unsigned *nsyms,
                       unsigned *log, unsigned max_log, unsigned max_syms);
int fse_write_ncount(sq_buf *out, const fse_norm *norm, unsigned nsyms, unsigned log);

/* Decoding table: for each state, its symbol, how many bits to read, and
 * the base those bits are added to for the next state. */
typedef struct {
    uint8_t symbol, nbits;
    uint16_t base;
} fse_dentry;

typedef struct {
    fse_dentry e[1 << FSE_MAX_LOG];
    unsigned log;
} fse_dtable;

int fse_build_dtable(fse_dtable *t, const fse_norm *norm, unsigned nsyms, unsigned log);

/* Encoding table: next-state lookup per symbol plus the per-symbol
 * constants that tell how many bits a state must shed. */
typedef struct {
    uint16_t next[1 << FSE_MAX_LOG];
    struct {
        int32_t find_state;
        uint32_t delta_nbits;
    } sym[FSE_MAX_SYMS];
    unsigned log;
} fse_ctable;

int fse_build_ctable(fse_ctable *t, const fse_norm *norm, unsigned nsyms, unsigned log);

/* ---- state machines ---- */

typedef struct {
    uint32_t state;
    const fse_dtable *t;
} fse_dstate;

static inline void fse_dstate_init(fse_dstate *s, const fse_dtable *t, bitreader_rev *br)
{
    s->t = t;
    s->state = (uint32_t)brr_read(br, t->log);
}

static inline unsigned fse_peek(const fse_dstate *s) { return s->t->e[s->state].symbol; }

static inline void fse_update(fse_dstate *s, bitreader_rev *br)
{
    const fse_dentry *e = &s->t->e[s->state];
    s->state = e->base + (uint32_t)brr_read(br, e->nbits);
}

typedef struct {
    uint32_t value;  /* in [2^log, 2^(log+1)) */
    const fse_ctable *t;
} fse_cstate;

/* Starts with the last symbol to be decoded, at no cost in bits. */
void fse_cstate_init(fse_cstate *s, const fse_ctable *t, unsigned symbol);
void fse_encode(fse_cstate *s, bitwriter_lsb *bw, unsigned symbol);
/* Writes the final state, which the decoder reads first. */
void fse_cstate_flush(const fse_cstate *s, bitwriter_lsb *bw);

/* ---- standalone order-0 codec (teaching; not a file format) ----
 * [8-byte length][table description][backward bit stream] */
int fse_compress(const uint8_t *in, size_t n, sq_buf *out);
int fse_decompress(const uint8_t *in, size_t n, sq_buf *out);

#endif
