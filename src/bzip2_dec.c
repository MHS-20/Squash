/* bzip2 decoder. A stream is "BZh" + block size digit, then blocks, each
 * introduced by a 48-bit magic, then an end-of-stream magic and the CRC of
 * all block CRCs. Everything is bit-packed MSB-first with no alignment until
 * the end of the stream.
 *
 * Block layout: CRC, randomised bit, BWT primary index (origPtr), the map
 * of byte values in use, table count, selectors (MTF + unary coded), the
 * code lengths of each table (delta coded), then the Huffman-coded
 * RUNA/RUNB/MTF symbols, switching table every 50 symbols. Undoing it runs
 * the encoder's pipeline backwards: RLE2, MTF, inverse BWT, RLE1. */
#include <stdlib.h>
#include <string.h>

#include "bitio.h"
#include "bwt.h"
#include "bzip2.h"
#include "checksum.h"
#include "huffman.h"
#include "mtf.h"
#include "rle.h"

typedef struct {
    bitreader_msb br;
    size_t max_block;       /* 100000 * level: bound on the BWT block */
    uint16_t *syms;         /* Huffman-decoded symbols of one block */
    uint8_t *mtf, *bwt, *block;
    huff_canon table[BZ_MAX_TABLES];
    uint8_t selector[BZ_MAX_SELECTORS];
} bz_decoder;

static uint64_t read48(bitreader_msb *br)
{
    uint64_t hi = brm_read(br, 24);
    return hi << 24 | brm_read(br, 24);
}

/* The byte values present, as a 16x16 bitmap with a 16-bit summary.
 * Returns how many are in use; seq_to_byte maps dense index -> byte. */
static int read_symbol_map(bitreader_msb *br, uint8_t *seq_to_byte)
{
    uint32_t groups = brm_read(br, 16);
    int n = 0;
    for (int g = 0; g < 16; g++) {
        if (!(groups & (0x8000u >> g)))
            continue;
        uint32_t bits = brm_read(br, 16);
        for (int i = 0; i < 16; i++)
            if (bits & (0x8000u >> i))
                seq_to_byte[n++] = (uint8_t)(g * 16 + i);
    }
    return n;
}

static int read_selectors(bz_decoder *d, int ntables, int *nsel_out)
{
    int nsel = (int)brm_read(&d->br, 15);
    if (nsel < 1)
        return SQ_ERR_DATA;
    uint8_t order[BZ_MAX_TABLES];
    for (int t = 0; t < ntables; t++)
        order[t] = (uint8_t)t;
    for (int i = 0; i < nsel; i++) {
        int j = 0;
        while (brm_read(&d->br, 1)) {  /* unary MTF index */
            if (++j >= ntables || d->br.overrun)
                return SQ_ERR_DATA;
        }
        /* Extra selectors beyond the most a 900k block can use are legal
         * but meaningless; bzip2 1.0.8 ignores them too. */
        if (i >= BZ_MAX_SELECTORS)
            continue;
        uint8_t t = order[j];
        memmove(order + 1, order, (size_t)j);
        order[0] = t;
        d->selector[i] = t;
    }
    *nsel_out = nsel < BZ_MAX_SELECTORS ? nsel : BZ_MAX_SELECTORS;
    return SQ_OK;
}

/* Lengths start at a 5-bit value; per symbol, "10" adds one, "11"
 * subtracts one, "0" moves to the next symbol. */
static int read_tables(bz_decoder *d, int ntables, int alpha)
{
    for (int t = 0; t < ntables; t++) {
        uint8_t len[BZ_MAX_ALPHA];
        int cur = (int)brm_read(&d->br, 5);
        for (int i = 0; i < alpha; i++) {
            for (;;) {
                if (cur < 1 || cur > BZ_MAX_CODE_LEN || d->br.overrun)
                    return SQ_ERR_DATA;
                if (!brm_read(&d->br, 1))
                    break;
                cur += brm_read(&d->br, 1) ? -1 : 1;
            }
            len[i] = (uint8_t)cur;
        }
        if (huff_canon_build(&d->table[t], len, alpha))
            return SQ_ERR_DATA;
    }
    return SQ_OK;
}

static int read_block(bz_decoder *d, sq_buf *out, uint32_t *crc_out)
{
    bitreader_msb *br = &d->br;
    uint32_t crc = brm_read(br, 32);
    int randomised = (int)brm_read(br, 1);
    int32_t primary = (int32_t)brm_read(br, 24);
    uint8_t seq_to_byte[256];
    int in_use = read_symbol_map(br, seq_to_byte);
    if (in_use == 0)
        return SQ_ERR_DATA;
    int alpha = in_use + 2, eob = in_use + 1;
    int ntables = (int)brm_read(br, 3), nsel;
    if (ntables < BZ_MIN_TABLES || ntables > BZ_MAX_TABLES)
        return SQ_ERR_DATA;
    int err = read_selectors(d, ntables, &nsel);
    if (!err)
        err = read_tables(d, ntables, alpha);
    if (err)
        return err;

    /* Huffman symbols up to EOB. Each symbol yields at least one output
     * byte except RUNA/RUNB digits, whose count is logarithmic, so the
     * buffer bound below is never reached by a valid block. */
    size_t nsyms = 0, cap = d->max_block + 64;
    for (int group = 0;; group++) {
        if (group >= nsel)
            return SQ_ERR_DATA;
        const huff_canon *h = &d->table[d->selector[group]];
        int sym = 0;
        for (int k = 0; k < BZ_GROUP_SIZE; k++) {
            sym = huff_decode_msb(h, br);
            if (sym < 0 || br->overrun || (sym != eob && nsyms == cap))
                return SQ_ERR_DATA;
            if (sym == eob)
                break;
            d->syms[nsyms++] = (uint16_t)sym;
        }
        if (sym == eob)
            break;
    }

    int64_t nblock = rle2_decode(d->syms, nsyms, d->mtf, d->max_block);
    if (nblock < 0)
        return SQ_ERR_DATA;
    mtf_decode(d->mtf, (size_t)nblock, d->bwt);
    for (int64_t i = 0; i < nblock; i++)
        d->bwt[i] = seq_to_byte[d->bwt[i]];  /* MTF indices are < in_use */
    err = bwt_inverse(d->bwt, (int32_t)nblock, primary, d->block);
    if (err)
        return err;
    if (randomised)
        bzip2_randomise(d->block, (size_t)nblock);
    size_t start = out->len;
    err = rle1_decode(d->block, (size_t)nblock, out);
    if (err)
        return err;
    if (sq_crc32_bzip2(0, out->data + start, out->len - start) != crc)
        return SQ_ERR_DATA;
    *crc_out = crc;
    return SQ_OK;
}

/* One stream; *used gets its length in bytes. */
static int stream(bz_decoder *d, const uint8_t *in, size_t n, sq_buf *out, size_t *used)
{
    if (n < 4 || in[0] != 'B' || in[1] != 'Z' || in[2] != 'h' || in[3] < '1' || in[3] > '9')
        return SQ_ERR_DATA;
    d->max_block = (size_t)(in[3] - '0') * 100000;
    brm_init(&d->br, in + 4, n - 4);
    uint32_t combined = 0;
    for (;;) {
        uint64_t magic = read48(&d->br);
        if (d->br.overrun)
            return SQ_ERR_DATA;
        if (magic == BZ_END_MAGIC)
            break;
        if (magic != BZ_BLOCK_MAGIC)
            return SQ_ERR_DATA;
        uint32_t crc;
        int err = read_block(d, out, &crc);
        if (err)
            return err;
        combined = ((combined << 1) | (combined >> 31)) ^ crc;
    }
    uint32_t want = brm_read(&d->br, 32);
    if (d->br.overrun || want != combined)
        return SQ_ERR_DATA;
    brm_align(&d->br);
    *used = 4 + brm_tell(&d->br, in + 4);
    return SQ_OK;
}

int sq_bzip2_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    bz_decoder *d = malloc(sizeof *d);
    if (!d)
        return SQ_ERR_MEM;
    /* Buffers sized for the largest block (level 9). */
    size_t max = 900000;
    d->syms = malloc(sizeof(uint16_t) * (max + 64));
    d->mtf = malloc(max);
    d->bwt = malloc(max);
    d->block = malloc(max);
    int err = d->syms && d->mtf && d->bwt && d->block ? SQ_OK : SQ_ERR_MEM;
    /* Concatenated streams (as from `cat a.bz2 b.bz2`) decode in order. */
    for (size_t pos = 0, used; !err && (pos < n || pos == 0); pos += used)
        err = stream(d, in + pos, n - pos, out, &used);
    free(d->syms);
    free(d->mtf);
    free(d->bwt);
    free(d->block);
    free(d);
    return err;
}
