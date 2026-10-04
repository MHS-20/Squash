/* bzip2 encoder: RLE1 -> BWT -> MTF -> RLE2 -> Huffman with up to six
 * tables chosen per 50-symbol group. */
#include <stdlib.h>
#include <string.h>

#include "bitio.h"
#include "bwt.h"
#include "bzip2.h"
#include "checksum.h"
#include "huffman.h"
#include "mtf.h"
#include "rle.h"

/* Refinement passes over the table assignment, as in bzip2. */
#define BZ_ITERATIONS 4

typedef struct {
    uint8_t len[BZ_MAX_TABLES][BZ_MAX_ALPHA];
    uint32_t code[BZ_MAX_TABLES][BZ_MAX_ALPHA];
    uint8_t selector[BZ_MAX_SELECTORS];
    int ntables, nsel;
} bz_tables;

/* More symbols justify more tables: each costs ~alpha*2 bits to send. */
static int table_count(size_t nsyms)
{
    return nsyms < 200 ? 2 : nsyms < 600 ? 3 : nsyms < 1200 ? 4 : nsyms < 2400 ? 5 : 6;
}

/* Starting point for the iteration: split the alphabet into ntables
 * contiguous ranges of roughly equal total frequency; each table makes its
 * range cheap (cost 0) and everything else expensive (cost 15). */
static void initial_tables(bz_tables *t, const uint32_t *freq, int alpha, size_t nsyms)
{
    size_t remaining = nsyms;
    int lo = 0;
    for (int part = t->ntables; part > 0; part--) {
        size_t target = remaining / (size_t)part, got = 0;
        int hi = lo - 1;
        while (got < target && hi < alpha - 1)
            got += freq[++hi];
        for (int s = 0; s < alpha; s++)
            t->len[part - 1][s] = (s >= lo && s <= hi) ? 0 : 15;
        lo = hi + 1;
        remaining -= got;
    }
}

/* Assign each group of 50 symbols the table that codes it cheapest, then
 * rebuild every table from the symbols assigned to it; repeat. This is
 * k-means style clustering of groups by symbol statistics. */
static void choose_tables(bz_tables *t, const uint16_t *syms, size_t nsyms, int alpha)
{
    t->nsel = (int)((nsyms + BZ_GROUP_SIZE - 1) / BZ_GROUP_SIZE);
    for (int it = 0; it < BZ_ITERATIONS; it++) {
        uint32_t freq[BZ_MAX_TABLES][BZ_MAX_ALPHA] = {{0}};
        for (int g = 0; g < t->nsel; g++) {
            size_t a = (size_t)g * BZ_GROUP_SIZE;
            size_t b = a + BZ_GROUP_SIZE < nsyms ? a + BZ_GROUP_SIZE : nsyms;
            int best = 0;
            uint32_t best_cost = UINT32_MAX;
            for (int k = 0; k < t->ntables; k++) {
                uint32_t cost = 0;
                for (size_t i = a; i < b; i++)
                    cost += t->len[k][syms[i]];
                if (cost < best_cost)
                    best_cost = cost, best = k;
            }
            t->selector[g] = (uint8_t)best;
            for (size_t i = a; i < b; i++)
                freq[best][syms[i]]++;
        }
        for (int k = 0; k < t->ntables; k++) {
            /* Every symbol needs a code: lengths are sent for the whole
             * alphabet and must be 1..20. */
            for (int s = 0; s < alpha; s++)
                freq[k][s] = freq[k][s] ? freq[k][s] : 1;
            huff_lengths(freq[k], alpha, BZ_ENC_CODE_LEN, t->len[k]);
        }
    }
    for (int k = 0; k < t->ntables; k++)
        huff_codes(t->len[k], alpha, t->code[k]);
}

static void write_tables(bitwriter_msb *bw, const bz_tables *t, int alpha)
{
    bwm_put(bw, (uint32_t)t->ntables, 3);
    bwm_put(bw, (uint32_t)t->nsel, 15);
    uint8_t order[BZ_MAX_TABLES];
    for (int k = 0; k < t->ntables; k++)
        order[k] = (uint8_t)k;
    for (int g = 0; g < t->nsel; g++) {  /* selectors: MTF, then unary */
        int j = 0;
        while (order[j] != t->selector[g])
            j++;
        memmove(order + 1, order, (size_t)j);
        order[0] = t->selector[g];
        for (int k = 0; k < j; k++)
            bwm_put(bw, 1, 1);
        bwm_put(bw, 0, 1);
    }
    for (int k = 0; k < t->ntables; k++) {  /* lengths, delta coded */
        int cur = t->len[k][0];
        bwm_put(bw, (uint32_t)cur, 5);
        for (int s = 0; s < alpha; s++) {
            for (; cur < t->len[k][s]; cur++)
                bwm_put(bw, 2, 2);  /* "10": +1 */
            for (; cur > t->len[k][s]; cur--)
                bwm_put(bw, 3, 2);  /* "11": -1 */
            bwm_put(bw, 0, 1);
        }
    }
}

typedef struct {
    uint8_t *bwt, *mtf;
    uint16_t *syms;
    bz_tables tables;
} bz_encoder;

static int write_block(bz_encoder *e, bitwriter_msb *bw, uint8_t *block, size_t n,
                       uint32_t crc, int randomise)
{
    if (randomise)
        bzip2_randomise(block, n);
    int32_t primary;
    int err = bwt_forward(block, (int32_t)n, e->bwt, &primary);
    if (err)
        return err;

    /* Dense alphabet of the bytes actually present. */
    int used[256] = {0}, in_use = 0;
    uint8_t byte_to_seq[256];
    for (size_t i = 0; i < n; i++)
        used[e->bwt[i]] = 1;
    for (int c = 0; c < 256; c++)
        if (used[c])
            byte_to_seq[c] = (uint8_t)in_use++;
    for (size_t i = 0; i < n; i++)
        e->bwt[i] = byte_to_seq[e->bwt[i]];
    mtf_encode(e->bwt, n, e->mtf);
    size_t nsyms = rle2_encode(e->mtf, n, e->syms);
    int alpha = in_use + 2;
    e->syms[nsyms++] = (uint16_t)(in_use + 1);  /* EOB */

    uint32_t freq[BZ_MAX_ALPHA] = {0};
    for (size_t i = 0; i < nsyms; i++)
        freq[e->syms[i]]++;
    bz_tables *t = &e->tables;
    t->ntables = table_count(nsyms);
    initial_tables(t, freq, alpha, nsyms);
    choose_tables(t, e->syms, nsyms, alpha);

    bwm_put(bw, (uint32_t)(BZ_BLOCK_MAGIC >> 24), 24);
    bwm_put(bw, (uint32_t)(BZ_BLOCK_MAGIC & 0xFFFFFF), 24);
    bwm_put(bw, crc, 32);
    bwm_put(bw, (uint32_t)randomise, 1);
    bwm_put(bw, (uint32_t)primary, 24);
    uint32_t groups = 0;
    for (int g = 0; g < 16; g++)
        for (int i = 0; i < 16; i++)
            if (used[g * 16 + i])
                groups |= 0x8000u >> g;
    bwm_put(bw, groups, 16);
    for (int g = 0; g < 16; g++) {
        if (!(groups & (0x8000u >> g)))
            continue;
        uint32_t bits = 0;
        for (int i = 0; i < 16; i++)
            if (used[g * 16 + i])
                bits |= 0x8000u >> i;
        bwm_put(bw, bits, 16);
    }
    write_tables(bw, t, alpha);
    for (size_t i = 0; i < nsyms; i++) {
        int k = t->selector[i / BZ_GROUP_SIZE];
        bwm_put(bw, t->code[k][e->syms[i]], t->len[k][e->syms[i]]);
    }
    return bw->err;
}

int bzip2_compress_ex(const uint8_t *in, size_t n, int level, int randomise, sq_buf *out)
{
    if (level == 0)
        level = 9;
    if (level < 1 || level > 9)
        return SQ_ERR_ARG;
    /* bzip2 stops 19 bytes short of the nominal size (bzlib.c nblockMAX). */
    size_t cap = (size_t)level * 100000 - 19;
    bz_encoder e;
    sq_buf block;
    sq_buf_init(&block);
    e.bwt = malloc(cap);
    e.mtf = malloc(cap);
    e.syms = malloc(sizeof(uint16_t) * (cap + 1));
    int err = e.bwt && e.mtf && e.syms ? SQ_OK : SQ_ERR_MEM;

    bitwriter_msb bw;
    bwm_init(&bw, out);
    bwm_put(&bw, 'B', 8);
    bwm_put(&bw, 'Z', 8);
    bwm_put(&bw, 'h', 8);
    bwm_put(&bw, (uint32_t)('0' + level), 8);
    uint32_t combined = 0;
    for (size_t pos = 0; !err && pos < n;) {
        size_t used;
        block.len = 0;
        err = rle1_encode(in + pos, n - pos, &used, &block, cap);
        if (err)
            break;
        uint32_t crc = sq_crc32_bzip2(0, in + pos, used);
        combined = ((combined << 1) | (combined >> 31)) ^ crc;
        err = write_block(&e, &bw, block.data, block.len, crc, randomise);
        pos += used;
    }
    if (!err) {
        bwm_put(&bw, (uint32_t)(BZ_END_MAGIC >> 24), 24);
        bwm_put(&bw, (uint32_t)(BZ_END_MAGIC & 0xFFFFFF), 24);
        bwm_put(&bw, combined, 32);
        err = bwm_flush(&bw);
    }
    sq_buf_free(&block);
    free(e.bwt);
    free(e.mtf);
    free(e.syms);
    return err;
}

int sq_bzip2_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    return bzip2_compress_ex(in, n, level, 0, out);
}
