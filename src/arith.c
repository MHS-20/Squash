/* The coder keeps an interval [low, high] of 32-bit code values. Coding a
 * symbol narrows it to the symbol's share of the cumulative frequencies.
 * Whenever both ends agree on the top bit, that bit is final and is shifted
 * out. If the interval straddles the middle while sitting inside the
 * middle half, the next bit is unknown but will be the opposite of the one
 * after it: those "pending" bits are counted and released later (the
 * underflow, or E3, case). */
#include <string.h>

#include "arith.h"
#include "bitio.h"

#define CODE_BITS 32
#define TOP ((1ull << CODE_BITS) - 1)
#define HALF (1ull << (CODE_BITS - 1))
#define QUARTER (1ull << (CODE_BITS - 2))
/* Frequencies must leave every symbol a non-empty slice of the smallest
 * possible interval (just over a quarter of the code space). */
#define MAX_TOTAL (1u << 16)
#define NSYM 256

typedef struct {
    uint32_t freq[NSYM];
    uint32_t total;
} model;

/* ---- models ---------------------------------------------------------- */

static void cum_range(const model *m, int s, uint32_t *lo, uint32_t *hi)
{
    uint32_t c = 0;
    for (int i = 0; i < s; i++)
        c += m->freq[i];
    *lo = c;
    *hi = c + m->freq[s];
}

/* Symbol whose cumulative range contains `target`. */
static int find_symbol(const model *m, uint32_t target, uint32_t *lo, uint32_t *hi)
{
    uint32_t c = 0;
    for (int s = 0; s < NSYM; s++) {
        if (target < c + m->freq[s]) {
            *lo = c;
            *hi = c + m->freq[s];
            return s;
        }
        c += m->freq[s];
    }
    return -1;
}

/* Adaptive update: favour recent symbols, and halve all counts when the
 * total gets too large, which also lets old statistics fade. */
static void adapt(model *m, int s)
{
    m->freq[s] += 32;
    m->total += 32;
    if (m->total > MAX_TOTAL) {
        m->total = 0;
        for (int i = 0; i < NSYM; i++) {
            m->freq[i] = (m->freq[i] + 1) / 2;
            m->total += m->freq[i];
        }
    }
}

static void adaptive_init(model *m)
{
    for (int i = 0; i < NSYM; i++)
        m->freq[i] = 1;
    m->total = NSYM;
}

/* ---- encoder --------------------------------------------------------- */

typedef struct {
    uint64_t low, high;
    uint64_t pending;
    bitwriter_msb bw;
} encoder;

static void put_bit_plus_pending(encoder *e, unsigned bit)
{
    bwm_put(&e->bw, bit, 1);
    for (; e->pending; e->pending--)
        bwm_put(&e->bw, !bit, 1);
}

static void encode(encoder *e, uint32_t lo, uint32_t hi, uint32_t total)
{
    uint64_t range = e->high - e->low + 1;
    e->high = e->low + range * hi / total - 1;
    e->low = e->low + range * lo / total;
    for (;;) {
        if (e->high < HALF) {
            put_bit_plus_pending(e, 0);
        } else if (e->low >= HALF) {
            put_bit_plus_pending(e, 1);
            e->low -= HALF;
            e->high -= HALF;
        } else if (e->low >= QUARTER && e->high < 3 * QUARTER) {
            e->pending++;
            e->low -= QUARTER;
            e->high -= QUARTER;
        } else {
            break;
        }
        e->low = 2 * e->low;
        e->high = 2 * e->high + 1;
    }
}

static int finish(encoder *e)
{
    /* Two more bits pin a value inside the final interval. */
    e->pending++;
    put_bit_plus_pending(e, e->low >= QUARTER);
    return bwm_flush(&e->bw);
}

/* ---- decoder --------------------------------------------------------- */

typedef struct {
    uint64_t low, high, value;
    bitreader_msb br;
} decoder;

static void decoder_init(decoder *d, const uint8_t *p, size_t n)
{
    brm_init(&d->br, p, n);
    d->low = 0;
    d->high = TOP;
    d->value = 0;
    /* Bits past the end read as zero: the encoder's final bits are chosen
     * so that any continuation stays inside the interval. */
    for (int i = 0; i < CODE_BITS; i++)
        d->value = (d->value << 1) | brm_read(&d->br, 1);
    d->br.overrun = 0;
}

static uint32_t target(const decoder *d, uint32_t total)
{
    uint64_t range = d->high - d->low + 1;
    return (uint32_t)(((d->value - d->low + 1) * total - 1) / range);
}

static void consume(decoder *d, uint32_t lo, uint32_t hi, uint32_t total)
{
    uint64_t range = d->high - d->low + 1;
    d->high = d->low + range * hi / total - 1;
    d->low = d->low + range * lo / total;
    for (;;) {
        if (d->high < HALF) {
            /* nothing to subtract */
        } else if (d->low >= HALF) {
            d->low -= HALF;
            d->high -= HALF;
            d->value -= HALF;
        } else if (d->low >= QUARTER && d->high < 3 * QUARTER) {
            d->low -= QUARTER;
            d->high -= QUARTER;
            d->value -= QUARTER;
        } else {
            break;
        }
        d->low = 2 * d->low;
        d->high = 2 * d->high + 1;
        d->value = 2 * d->value | brm_read(&d->br, 1);
    }
}

/* ---- public ---------------------------------------------------------- */

static int run_encoder(const uint8_t *in, size_t n, model *m, int adaptive, sq_buf *out)
{
    encoder e = {0, TOP, 0, {0}};
    bwm_init(&e.bw, out);
    for (size_t i = 0; i < n; i++) {
        uint32_t lo, hi;
        cum_range(m, in[i], &lo, &hi);
        encode(&e, lo, hi, m->total);
        if (adaptive)
            adapt(m, in[i]);
    }
    return finish(&e);
}

static int run_decoder(const uint8_t *in, size_t n, model *m, int adaptive, uint64_t count,
                       sq_buf *out)
{
    decoder d;
    decoder_init(&d, in, n);
    /* A near-certain symbol costs almost no bits, so the declared count is
     * not bounded by the input size; the output buffer's cap bounds it. */
    int err = sq_buf_reserve(out, count < (1u << 20) ? count : (1u << 20));
    for (uint64_t i = 0; !err && i < count; i++) {
        uint32_t lo, hi;
        uint32_t t = target(&d, m->total);
        int s = t < m->total ? find_symbol(m, t, &lo, &hi) : -1;
        if (s < 0)
            return SQ_ERR_DATA;
        consume(&d, lo, hi, m->total);
        err = sq_buf_push(out, (uint8_t)s);
        if (adaptive)
            adapt(m, s);
    }
    return err;
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* Scale counts into 1..MAX_TOTAL total, keeping every present symbol. */
static void static_model(const uint8_t *in, size_t n, model *m)
{
    uint64_t count[NSYM] = {0};
    for (size_t i = 0; i < n; i++)
        count[in[i]]++;
    m->total = 0;
    for (int s = 0; s < NSYM; s++) {
        uint64_t f = count[s] * (MAX_TOTAL - NSYM) / (n ? n : 1);
        m->freq[s] = count[s] ? (uint32_t)f + 1 : 0;
        m->total += m->freq[s];
    }
}

int arith_static_compress(const uint8_t *in, size_t n, sq_buf *out)
{
    model m;
    static_model(in, n, &m);
    int err = sq_buf_put_le(out, n, 8);
    for (int s = 0; !err && s < NSYM; s++)
        err = sq_buf_put_le(out, m.freq[s], 2);
    return err ? err : run_encoder(in, n, &m, 0, out);
}

int arith_static_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    if (n < 8 + 2 * NSYM)
        return SQ_ERR_DATA;
    model m;
    m.total = 0;
    for (int s = 0; s < NSYM; s++) {
        m.freq[s] = in[8 + 2 * s] | (uint32_t)in[9 + 2 * s] << 8;
        m.total += m.freq[s];
    }
    uint64_t count = le64(in);
    if (m.total > MAX_TOTAL || (count && m.total == 0))
        return SQ_ERR_DATA;
    return run_decoder(in + 8 + 2 * NSYM, n - 8 - 2 * NSYM, &m, 0, count, out);
}

int arith_adaptive_compress(const uint8_t *in, size_t n, sq_buf *out)
{
    model m;
    adaptive_init(&m);
    int err = sq_buf_put_le(out, n, 8);
    return err ? err : run_encoder(in, n, &m, 1, out);
}

int arith_adaptive_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    if (n < 8)
        return SQ_ERR_DATA;
    model m;
    adaptive_init(&m);
    return run_decoder(in + 8, n - 8, &m, 1, le64(in), out);
}
