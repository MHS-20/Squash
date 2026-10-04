#include "fse.h"

#include <string.h>

static unsigned highbit(uint32_t v) { return 31 - (unsigned)__builtin_clz(v); }

/* ---- normalisation --------------------------------------------------- */

int fse_normalize(const uint32_t *count, unsigned nsyms, size_t total, unsigned log,
                  int allow_low, fse_norm *norm)
{
    if (total == 0 || log < FSE_MIN_LOG || log > FSE_MAX_LOG)
        return SQ_ERR_ARG;
    int32_t size = 1 << log, used = 0;
    unsigned present = 0, top = 0;
    /* Proportional share, rounded, at least 1; a symbol whose share is
     * below one state becomes -1 when allowed (it still takes a state). */
    for (unsigned s = 0; s < nsyms; s++) {
        norm[s] = 0;
        if (!count[s])
            continue;
        present++;
        if (count[s] > count[top])
            top = s;
        uint64_t scaled = (uint64_t)count[s] * (uint64_t)size;
        int32_t share = (int32_t)((scaled + total / 2) / total);
        norm[s] = allow_low && scaled < total ? -1 : (fse_norm)(share < 1 ? 1 : share);
        used += norm[s] < 0 ? 1 : norm[s];
    }
    if (present > (unsigned)size)
        return SQ_ERR_ARG;  /* more symbols than states */
    if (norm[top] < 0) {
        norm[top] = 1;  /* the most frequent symbol must be adjustable */
    }
    /* Rounding leaves the sum a little off; settle the difference on the
     * symbols with the most states, where it costs the least. */
    while (used != size) {
        unsigned best = top;
        for (unsigned s = 0; s < nsyms; s++)
            if (norm[s] > norm[best])
                best = s;
        if (used < size) {
            norm[best]++;
            used++;
        } else {
            if (norm[best] <= 1)
                return SQ_ERR_ARG;
            norm[best]--;
            used--;
        }
    }
    return SQ_OK;
}

/* ---- table description ----------------------------------------------- */

/* Counts are written in turn with a variable number of bits: only values
 * up to the remaining probability are possible, so the bit width shrinks
 * as the total is used up, and small values take one bit fewer. A zero
 * count is followed by 2-bit repeat flags for further zeros. */
int fse_write_ncount(sq_buf *out, const fse_norm *norm, unsigned nsyms, unsigned log)
{
    bitwriter_lsb bw;
    bwl_init(&bw, out);
    bwl_put(&bw, log - FSE_MIN_LOG, 4);
    int remaining = (1 << log) + 1, threshold = 1 << log;
    unsigned nbits = log + 1, s = 0;
    while (s < nsyms && remaining > 1) {
        int count = norm[s++];
        int max = 2 * threshold - 1 - remaining;
        remaining -= count < 0 ? -count : count;
        uint32_t v = (uint32_t)(count + 1);
        if (v >= (uint32_t)threshold)
            v += (uint32_t)max;
        bwl_put(&bw, v, nbits - (v < (uint32_t)max ? 1 : 0));
        while (remaining < threshold) {
            nbits--;
            threshold >>= 1;
        }
        if (count == 0) {
            unsigned start = s;
            while (s < nsyms && norm[s] == 0)
                s++;
            unsigned run = s - start;
            for (; run >= 3; run -= 3)
                bwl_put(&bw, 3, 2);
            bwl_put(&bw, run, 2);
        }
    }
    return bwl_flush(&bw);
}

size_t fse_read_ncount(const uint8_t *in, size_t n, fse_norm *norm, unsigned *nsyms,
                       unsigned *log, unsigned max_log, unsigned max_syms)
{
    bitreader_lsb br;
    brl_init(&br, in, n);
    unsigned lg = brl_read(&br, 4) + FSE_MIN_LOG;
    if (lg > max_log)
        return 0;
    int remaining = (1 << lg) + 1, threshold = 1 << lg;
    unsigned nbits = lg + 1, s = 0;
    size_t used_bits = 4;
    memset(norm, 0, sizeof(fse_norm) * max_syms);
    while (remaining > 1) {
        if (s >= max_syms)
            return 0;
        int max = 2 * threshold - 1 - remaining;
        int count;
        uint32_t low = brl_peek(&br, nbits - 1);
        if (low < (uint32_t)max) {
            count = (int)low;
            brl_consume(&br, nbits - 1);
            used_bits += nbits - 1;
        } else {
            uint32_t v = brl_peek(&br, nbits);
            count = (int)(v >= (uint32_t)threshold ? v - (uint32_t)max : v);
            brl_consume(&br, nbits);
            used_bits += nbits;
        }
        count--;  /* stored values are shifted by one so -1 fits */
        if (count > remaining - 1)
            return 0;
        remaining -= count < 0 ? -count : count;
        norm[s++] = (fse_norm)count;
        if (remaining <= 1)
            break;
        while (remaining < threshold) {
            nbits--;
            threshold >>= 1;
        }
        if (count == 0) {
            uint32_t r;
            do {
                r = brl_read(&br, 2);
                used_bits += 2;
                s += r;
            } while (r == 3);
            if (s > max_syms)
                return 0;
        }
        if (br.overrun)
            return 0;
    }
    if (remaining != 1 || br.overrun)
        return 0;
    *nsyms = s;
    *log = lg;
    return (used_bits + 7) / 8;
}

/* ---- tables ---------------------------------------------------------- */

/* Scatter symbols over the table with a step coprime to its size, so
 * each symbol's states are spread out; -1 symbols take the last states.
 * Encoder and decoder must agree on this layout exactly. */
static int spread(const fse_norm *norm, unsigned nsyms, unsigned log, uint8_t *symbol,
                  uint32_t *high)
{
    uint32_t size = 1u << log, mask = size - 1, step = (size >> 1) + (size >> 3) + 3;
    uint32_t hi = size - 1, pos = 0, total = 0;
    for (unsigned s = 0; s < nsyms; s++) {
        if (norm[s] == -1) {
            symbol[hi--] = (uint8_t)s;
            total++;
        } else if (norm[s] < -1) {
            return SQ_ERR_DATA;
        } else {
            total += (uint32_t)norm[s];
        }
    }
    if (total != size)
        return SQ_ERR_DATA;
    for (unsigned s = 0; s < nsyms; s++) {
        for (int i = 0; i < norm[s]; i++) {
            symbol[pos] = (uint8_t)s;
            do
                pos = (pos + step) & mask;
            while (pos > hi);
        }
    }
    *high = hi;
    return pos == 0 ? SQ_OK : SQ_ERR_DATA;
}

int fse_build_dtable(fse_dtable *t, const fse_norm *norm, unsigned nsyms, unsigned log)
{
    if (log > FSE_MAX_LOG || nsyms > FSE_MAX_SYMS)
        return SQ_ERR_DATA;
    uint8_t symbol[1 << FSE_MAX_LOG];
    uint32_t hi, next[FSE_MAX_SYMS];
    int err = spread(norm, nsyms, log, symbol, &hi);
    if (err)
        return err;
    for (unsigned s = 0; s < nsyms; s++)
        next[s] = norm[s] == -1 ? 1 : (uint32_t)norm[s];
    uint32_t size = 1u << log;
    /* A symbol with k states numbers them k..2k-1 in table order; state
     * number x reads log - floor(log2 x) bits, which lands the next state
     * anywhere in a range of matching width. */
    for (uint32_t u = 0; u < size; u++) {
        unsigned s = symbol[u];
        uint32_t x = next[s]++;
        unsigned nbits = log - highbit(x);
        t->e[u].symbol = (uint8_t)s;
        t->e[u].nbits = (uint8_t)nbits;
        t->e[u].base = (uint16_t)((x << nbits) - size);
    }
    t->log = log;
    return SQ_OK;
}

int fse_build_ctable(fse_ctable *t, const fse_norm *norm, unsigned nsyms, unsigned log)
{
    if (log > FSE_MAX_LOG || nsyms > FSE_MAX_SYMS)
        return SQ_ERR_ARG;
    uint8_t symbol[1 << FSE_MAX_LOG];
    uint32_t hi, cumul[FSE_MAX_SYMS + 1];
    int err = spread(norm, nsyms, log, symbol, &hi);
    if (err)
        return err;
    uint32_t size = 1u << log;
    cumul[0] = 0;
    for (unsigned s = 0; s < nsyms; s++)
        cumul[s + 1] = cumul[s] + (norm[s] == -1 ? 1 : (uint32_t)norm[s]);
    /* For each symbol, its states in table order: where to go next. */
    for (uint32_t u = 0; u < size; u++)
        t->next[cumul[symbol[u]]++] = (uint16_t)(size + u);
    uint32_t total = 0;
    for (unsigned s = 0; s < FSE_MAX_SYMS; s++) {
        int c = s < nsyms ? norm[s] : 0;
        if (c == 0) {
            t->sym[s].delta_nbits = ((log + 1) << 16) - size;
            t->sym[s].find_state = 0;
        } else if (c == -1 || c == 1) {
            t->sym[s].delta_nbits = (log << 16) - size;
            t->sym[s].find_state = (int32_t)total - 1;
            total++;
        } else {
            /* States below c << max_bits shed one bit fewer. Folding that
             * threshold into delta_nbits lets the encoder find the bit
             * count with one add and shift. */
            unsigned max_bits = log - highbit((uint32_t)c - 1);
            uint32_t min_state_plus = (uint32_t)c << max_bits;
            t->sym[s].delta_nbits = (max_bits << 16) - min_state_plus;
            t->sym[s].find_state = (int32_t)total - c;
            total += (uint32_t)c;
        }
    }
    t->log = log;
    return SQ_OK;
}

void fse_cstate_init(fse_cstate *s, const fse_ctable *t, unsigned symbol)
{
    s->t = t;
    uint32_t nbits = (t->sym[symbol].delta_nbits + (1u << 15)) >> 16;
    uint32_t value = (nbits << 16) - t->sym[symbol].delta_nbits;
    s->value = t->next[(int32_t)(value >> nbits) + t->sym[symbol].find_state];
}

void fse_encode(fse_cstate *s, bitwriter_lsb *bw, unsigned symbol)
{
    uint32_t nbits = (s->value + s->t->sym[symbol].delta_nbits) >> 16;
    bwl_put(bw, s->value, nbits);
    s->value = s->t->next[(int32_t)(s->value >> nbits) + s->t->sym[symbol].find_state];
}

void fse_cstate_flush(const fse_cstate *s, bitwriter_lsb *bw)
{
    bwl_put(bw, s->value, s->t->log);  /* the low log bits: value - 2^log */
}

/* ---- standalone codec ------------------------------------------------ */

#define CODEC_LOG 11

int fse_compress(const uint8_t *in, size_t n, sq_buf *out)
{
    int err = sq_buf_put_le(out, n, 8);
    if (err || n == 0)
        return err;
    uint32_t count[FSE_MAX_SYMS] = {0};
    for (size_t i = 0; i < n; i++)
        count[in[i]]++;
    unsigned nsyms = FSE_MAX_SYMS;
    while (!count[nsyms - 1])
        nsyms--;
    fse_norm norm[FSE_MAX_SYMS];
    static _Thread_local fse_ctable ct;
    if ((err = fse_normalize(count, nsyms, n, CODEC_LOG, 1, norm)) ||
        (err = fse_write_ncount(out, norm, nsyms, CODEC_LOG)) ||
        (err = fse_build_ctable(&ct, norm, nsyms, CODEC_LOG)))
        return err;
    bitwriter_lsb bw;
    bwl_init(&bw, out);
    fse_cstate cs;
    fse_cstate_init(&cs, &ct, in[n - 1]);
    for (size_t i = n - 1; i-- > 0; )
        fse_encode(&cs, &bw, in[i]);
    fse_cstate_flush(&cs, &bw);
    return bwl_close_rev(&bw);
}

int fse_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    if (n < 8)
        return SQ_ERR_DATA;
    uint64_t len = 0;
    for (int i = 7; i >= 0; i--)
        len = len << 8 | in[i];
    if (len == 0)
        return n == 8 ? SQ_OK : SQ_ERR_DATA;
    fse_norm norm[FSE_MAX_SYMS];
    unsigned nsyms, log;
    size_t h = fse_read_ncount(in + 8, n - 8, norm, &nsyms, &log, FSE_MAX_LOG, FSE_MAX_SYMS);
    static _Thread_local fse_dtable dt;
    if (!h || fse_build_dtable(&dt, norm, nsyms, log))
        return SQ_ERR_DATA;
    bitreader_rev br;
    if (brr_init(&br, in + 8 + h, n - 8 - h))
        return SQ_ERR_DATA;
    fse_dstate ds;
    fse_dstate_init(&ds, &dt, &br);
    for (uint64_t i = 0; i < len; i++) {
        int err = sq_buf_push(out, (uint8_t)fse_peek(&ds));
        if (err)
            return err;
        if (i + 1 < len)
            fse_update(&ds, &br);
        if (br.overrun)
            return SQ_ERR_DATA;
    }
    return br.bits == 0 ? SQ_OK : SQ_ERR_DATA;  /* every bit accounted for */
}
