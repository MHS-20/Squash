#include "rans.h"

#include <stdlib.h>
#include <string.h>

#include "fse.h"

/* The state lives in [L, 256 L): renormalising a byte at a time keeps it
 * there, and L = 2^23 keeps 256 L within 32 bits. */
#define RANS_L (1u << 23)
#define SCALE (1u << RANS_SCALE_BITS)

typedef struct {
    uint32_t freq[256], start[257];
    uint16_t slot[SCALE];  /* cumulative position -> symbol */
} rans_model;

static void finish_model(rans_model *m)
{
    m->start[0] = 0;
    for (int s = 0; s < 256; s++)
        m->start[s + 1] = m->start[s] + m->freq[s];
    for (int s = 0; s < 256; s++)
        for (uint32_t i = m->start[s]; i < m->start[s + 1]; i++)
            m->slot[i] = (uint16_t)s;
}

/* Frequencies summing exactly to 2^scale; FSE's quantiser does it (with
 * every present symbol kept >= 1, since rANS has no "below one" trick). */
static int build_model(const uint8_t *in, size_t n, rans_model *m)
{
    uint32_t count[256] = {0};
    for (size_t i = 0; i < n; i++)
        count[in[i]]++;
    fse_norm norm[256];
    int err = fse_normalize(count, 256, n, RANS_SCALE_BITS, 0, norm);
    if (err)
        return err;
    for (int s = 0; s < 256; s++)
        m->freq[s] = (uint32_t)norm[s];
    finish_model(m);
    return SQ_OK;
}

static int write_header(sq_buf *out, size_t n, const rans_model *m)
{
    int err = sq_buf_put_le(out, n, 8);
    for (int s = 0; !err && s < 256; s++)
        err = sq_buf_put_le(out, m->freq[s], 2);
    return err;
}

static int read_header(const uint8_t *in, size_t n, uint64_t *len, rans_model *m)
{
    if (n < 8 + 512)
        return SQ_ERR_DATA;
    *len = 0;
    for (int i = 7; i >= 0; i--)
        *len = *len << 8 | in[i];
    uint32_t total = 0;
    for (int s = 0; s < 256; s++) {
        m->freq[s] = in[8 + 2 * s] | (uint32_t)in[9 + 2 * s] << 8;
        total += m->freq[s];
    }
    if (*len && total != SCALE)
        return SQ_ERR_DATA;
    if (*len)
        finish_model(m);
    return SQ_OK;
}

/* ---- coding steps ---------------------------------------------------- */

/* Pushes bytes downward from *p (the stream is built back to front). */
static void enc_put(uint32_t *x, uint8_t **p, const rans_model *m, uint8_t s)
{
    uint32_t f = m->freq[s];
    /* Shed low bytes until encoding cannot overflow the state's range. */
    uint32_t x_max = ((RANS_L >> RANS_SCALE_BITS) << 8) * f;
    while (*x >= x_max) {
        *--*p = (uint8_t)*x;
        *x >>= 8;
    }
    *x = ((*x / f) << RANS_SCALE_BITS) + (*x % f) + m->start[s];
}

/* The final state, big-endian so the decoder can shift it in. */
static void enc_flush(uint32_t x, uint8_t **p)
{
    for (int i = 0; i < 4; i++)
        *--*p = (uint8_t)(x >> (8 * i));
}

typedef struct {
    const uint8_t *p, *end;
    int overrun;
} rans_in;

static uint8_t next_byte(rans_in *r)
{
    if (r->p == r->end) {
        r->overrun = 1;
        return 0;
    }
    return *r->p++;
}

static uint32_t dec_init(rans_in *r)
{
    uint32_t x = 0;
    for (int i = 0; i < 4; i++)
        x = x << 8 | next_byte(r);
    return x;
}

static uint8_t dec_get(uint32_t *x, rans_in *r, const rans_model *m)
{
    uint8_t s = (uint8_t)m->slot[*x & (SCALE - 1)];
    *x = m->freq[s] * (*x >> RANS_SCALE_BITS) + (*x & (SCALE - 1)) - m->start[s];
    while (*x < RANS_L)
        *x = *x << 8 | next_byte(r);
    return s;
}

/* ---- codecs ---------------------------------------------------------- */

static int compress(const uint8_t *in, size_t n, sq_buf *out, int ways)
{
    rans_model *m = malloc(sizeof *m);
    if (!m)
        return SQ_ERR_MEM;
    int err = n ? build_model(in, n, m) : (memset(m, 0, sizeof *m), SQ_OK);
    if (!err)
        err = write_header(out, n, m);
    /* Worst case ~ (scale bits + 1) per symbol plus the states. */
    size_t cap = n * 2 + 16;
    uint8_t *buf = err ? NULL : malloc(cap);
    if (!err && !buf)
        err = SQ_ERR_MEM;
    if (!err && n) {
        uint8_t *p = buf + cap;
        uint32_t x[2] = {RANS_L, RANS_L};
        for (size_t i = n; i-- > 0; )
            enc_put(&x[i % ways], &p, m, in[i]);
        for (int w = ways - 1; w >= 0; w--)
            enc_flush(x[w], &p);
        err = sq_buf_append(out, p, (size_t)(buf + cap - p));
    }
    free(buf);
    free(m);
    return err;
}

static int decompress(const uint8_t *in, size_t n, sq_buf *out, int ways)
{
    rans_model *m = malloc(sizeof *m);
    if (!m)
        return SQ_ERR_MEM;
    uint64_t len;
    int err = read_header(in, n, &len, m);
    if (!err && len) {
        rans_in r = {in + 8 + 512, in + n, 0};
        uint32_t x[2];
        for (int w = 0; w < ways; w++)
            x[w] = dec_init(&r);
        for (uint64_t i = 0; !err && i < len; i++) {
            err = sq_buf_push(out, dec_get(&x[i % (uint64_t)ways], &r, m));
            if (r.overrun)
                err = SQ_ERR_DATA;
        }
        /* The encoder started each state at L; ending anywhere else, or
         * with bytes left over, means corruption. */
        for (int w = 0; !err && w < ways; w++)
            if (x[w] != RANS_L)
                err = SQ_ERR_DATA;
        if (!err && r.p != r.end)
            err = SQ_ERR_DATA;
    }
    free(m);
    return err;
}

int rans_compress(const uint8_t *in, size_t n, sq_buf *out) { return compress(in, n, out, 1); }
int rans_decompress(const uint8_t *in, size_t n, sq_buf *out) { return decompress(in, n, out, 1); }
int rans2_compress(const uint8_t *in, size_t n, sq_buf *out) { return compress(in, n, out, 2); }
int rans2_decompress(const uint8_t *in, size_t n, sq_buf *out) { return decompress(in, n, out, 2); }
