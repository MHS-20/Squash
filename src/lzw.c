/* LZW in the Unix `compress` (.Z) format.
 *
 * Stream: 0x1F 0x9D, a flags byte (max code width in the low 5 bits, 0x80 =
 * "block mode", which reserves code 256 as CLEAR), then codes packed
 * LSB-first. Codes start 9 bits wide and widen as the dictionary grows,
 * up to the maximum (9..16).
 *
 * The quirk every implementation must copy: compress emits codes in groups
 * of 8, and whenever the width changes (or after CLEAR) the rest of the
 * current group is skipped. Groups are n_bits bytes long, so the padding
 * realigns the stream to a multiple of n_bits*8 bits counted from where the
 * previous width began. */
#include <stdlib.h>
#include <string.h>

#include "bitio.h"
#include "squash.h"

#define LZW_CLEAR 256
#define LZW_FIRST 257
#define LZW_BLOCK_MODE 0x80
#define LZW_MIN_BITS 9
#define LZW_MAX_BITS 16
/* Once the table is full, compress re-checks the ratio this often and
 * starts a fresh table (CLEAR) as soon as it stops improving. */
#define LZW_CHECK_GAP 10000

/* Bits to skip so that `pos` is a whole number of n_bits*8-bit groups past
 * `group_start`. */
static uint64_t group_padding(uint64_t pos, uint64_t group_start, unsigned n_bits)
{
    uint64_t g = (uint64_t)n_bits * 8, used = (pos - group_start) % g;
    return used ? g - used : 0;
}

/* ---- decoder --------------------------------------------------------- */

typedef struct {
    uint16_t prefix[1 << LZW_MAX_BITS];
    uint8_t suffix[1 << LZW_MAX_BITS];
    uint8_t stack[(1 << LZW_MAX_BITS) + 1];
} lzw_table;

static void skip_bits(bitreader_lsb *br, uint64_t n)
{
    for (; n > 16; n -= 16)
        brl_read(br, 16);
    brl_read(br, (unsigned)n);
}

int sq_lzw_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    if (n < 3 || in[0] != 0x1F || in[1] != 0x9D)
        return SQ_ERR_DATA;
    unsigned max_bits = in[2] & 0x1F;
    int block_mode = (in[2] & LZW_BLOCK_MODE) != 0;
    if (max_bits < LZW_MIN_BITS || max_bits > LZW_MAX_BITS)
        return SQ_ERR_DATA;
    lzw_table *t = malloc(sizeof *t);
    if (!t)
        return SQ_ERR_MEM;
    for (int i = 0; i < 256; i++)
        t->suffix[i] = (uint8_t)i;

    bitreader_lsb br;
    brl_init(&br, in + 3, n - 3);
    uint64_t total = (uint64_t)(n - 3) * 8, pos = 0, group_start = 0;
    unsigned n_bits = LZW_MIN_BITS;
    /* The decoder learns each entry one code later than the encoder made
     * it (it needs the next string's first byte), so its `next_free` trails
     * the encoder's by one and it widens at 2^n rather than 2^n + 1. */
    uint32_t next_free = block_mode ? LZW_FIRST : 256;
    int32_t old = -1;
    uint8_t first_byte = 0;
    int err = SQ_OK;

    while (!err) {
        uint32_t max_code = n_bits == max_bits ? 1u << max_bits : (1u << n_bits) - 1;
        if (next_free > max_code) {
            uint64_t pad = group_padding(pos, group_start, n_bits);
            skip_bits(&br, pad);
            pos += pad;
            group_start = pos;
            n_bits++;
            continue;
        }
        if (pos + n_bits > total)
            break;  /* fewer bits left than a code: end of stream */
        uint32_t code = brl_read(&br, n_bits);
        pos += n_bits;

        if (old < 0) {
            if (code >= 256) {
                err = SQ_ERR_DATA;
                break;
            }
            first_byte = (uint8_t)code;
            old = (int32_t)code;
            err = sq_buf_push(out, first_byte);
            continue;
        }
        if (code == LZW_CLEAR && block_mode) {
            /* Like compress, the next code re-creates slot 256 harmlessly,
             * so counting restarts one below LZW_FIRST. */
            uint64_t pad = group_padding(pos, group_start, n_bits);
            skip_bits(&br, pad);
            pos += pad;
            group_start = pos;
            n_bits = LZW_MIN_BITS;
            next_free = LZW_CLEAR;
            continue;
        }

        uint32_t in_code = code;
        size_t sp = sizeof t->stack;
        if (code >= next_free) {
            /* The KwKwK case: the code being defined right now. Its string
             * is the previous one plus its own first byte. */
            if (code > next_free) {
                err = SQ_ERR_DATA;
                break;
            }
            t->stack[--sp] = first_byte;
            code = (uint32_t)old;
        }
        /* Prefix links always point to smaller codes, so this terminates
         * and needs at most one stack slot per table entry. */
        while (code >= 256) {
            t->stack[--sp] = t->suffix[code];
            code = t->prefix[code];
        }
        first_byte = (uint8_t)code;
        t->stack[--sp] = first_byte;
        err = sq_buf_append(out, t->stack + sp, sizeof t->stack - sp);

        if (next_free < (1u << max_bits)) {
            t->prefix[next_free] = (uint16_t)old;
            t->suffix[next_free] = first_byte;
            next_free++;
        }
        old = (int32_t)in_code;
    }
    free(t);
    return err;
}

/* ---- encoder --------------------------------------------------------- */

/* Dictionary lookup (prefix code, next byte) -> code, open addressing. */
#define DICT_BITS 18
typedef struct {
    uint32_t key[1 << DICT_BITS];  /* (prefix << 8 | byte) + 1; 0 = empty */
    uint16_t code[1 << DICT_BITS];
} lzw_dict;

static uint32_t *dict_slot(lzw_dict *d, uint32_t key, uint16_t **code)
{
    uint32_t h = (key * 2654435761u) >> (32 - DICT_BITS);
    while (d->key[h] && d->key[h] != key)
        h = (h + 1) & ((1u << DICT_BITS) - 1);
    *code = &d->code[h];
    return &d->key[h];
}

typedef struct {
    bitwriter_lsb bw;
    uint64_t pos, group_start;
    unsigned n_bits;
} code_writer;

static void put_code(code_writer *w, uint32_t code)
{
    bwl_put(&w->bw, code, w->n_bits);
    w->pos += w->n_bits;
}

static void pad_group(code_writer *w)
{
    for (uint64_t pad = group_padding(w->pos, w->group_start, w->n_bits); pad; ) {
        unsigned k = pad > 16 ? 16 : (unsigned)pad;
        bwl_put(&w->bw, 0, k);
        pad -= k;
    }
    w->pos += group_padding(w->pos, w->group_start, w->n_bits);
    w->group_start = w->pos;
}

/* `level` is the maximum code width, as in `compress -b`: 9..16. */
int sq_lzw_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    unsigned max_bits = level ? (unsigned)level : LZW_MAX_BITS;
    if (max_bits < LZW_MIN_BITS || max_bits > LZW_MAX_BITS)
        return SQ_ERR_ARG;
    int err = sq_buf_push(out, 0x1F);
    if (!err)
        err = sq_buf_push(out, 0x9D);
    if (!err)
        err = sq_buf_push(out, (uint8_t)(max_bits | LZW_BLOCK_MODE));
    if (err || n == 0)
        return err;
    lzw_dict *d = calloc(1, sizeof *d);
    if (!d)
        return SQ_ERR_MEM;

    code_writer w = {.n_bits = LZW_MIN_BITS};
    bwl_init(&w.bw, out);
    size_t out_start = out->len;
    uint32_t next_free = LZW_FIRST, ent = in[0];
    size_t checkpoint = LZW_CHECK_GAP;
    uint64_t best_ratio = 0;

    for (size_t i = 1; i < n; i++) {
        uint32_t key = (ent << 8 | in[i]) + 1;
        uint16_t *code;
        uint32_t *slot = dict_slot(d, key, &code);
        if (*slot) {
            ent = *code;  /* keep extending the current string */
            continue;
        }
        /* Widen one code later than the table needs it: see the decoder. */
        if (w.n_bits < max_bits && next_free > (1u << w.n_bits)) {
            pad_group(&w);
            w.n_bits++;
        }
        put_code(&w, ent);
        ent = in[i];
        if (next_free < (1u << max_bits)) {
            *slot = key;
            *code = (uint16_t)next_free++;
            continue;
        }
        /* Table full: keep using it while the ratio (bytes in per byte out,
         * 8 fractional bits) holds up; otherwise start over. */
        if (i >= checkpoint) {
            checkpoint = i + LZW_CHECK_GAP;
            uint64_t bytes_out = out->len - out_start + 1;
            uint64_t ratio = ((uint64_t)i << 8) / bytes_out;
            if (ratio >= best_ratio) {
                best_ratio = ratio;
            } else {
                best_ratio = 0;
                put_code(&w, LZW_CLEAR);
                pad_group(&w);
                w.n_bits = LZW_MIN_BITS;
                next_free = LZW_FIRST;
                memset(d->key, 0, sizeof d->key);
            }
        }
    }
    if (w.n_bits < max_bits && next_free > (1u << w.n_bits)) {
        pad_group(&w);
        w.n_bits++;
    }
    put_code(&w, ent);
    free(d);
    return bwl_flush(&w.bw);
}
