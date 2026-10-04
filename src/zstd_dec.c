/* zstd decoder (RFC 8878): frames of blocks; a compressed block holds a
 * literals section (raw, RLE or Huffman) and a sequences section whose
 * literal lengths, offsets and match lengths are FSE-coded together in one
 * backward bit stream. Decoding a block replays its sequences: copy some
 * literals, then copy a match from earlier output. */
#include <stdlib.h>
#include <string.h>

#include "checksum.h"
#include "zstd.h"

typedef struct {
    sq_buf w;              /* dictionary content, then this frame's output */
    size_t floor;          /* where the frame's own output starts in w */
    size_t block_max;
    uint32_t rep[3];
    huf0_dtable huf;
    int have_huf;
    fse_dtable seq[3];     /* last tables, for Repeat_Mode */
    int have_seq[3];
    uint8_t lit[ZSTD_BLOCK_MAX];
} zstd_dctx;

static uint32_t le(const uint8_t *p, int bytes)
{
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* ---- literals -------------------------------------------------------- */

/* Returns the section's size, or 0 on error; *nlit gets the literal count. */
static size_t literals(zstd_dctx *d, const uint8_t *in, size_t n, size_t *nlit)
{
    if (n < 1)
        return 0;
    unsigned type = in[0] & 3, sf = (in[0] >> 2) & 3;
    if (type == ZSTD_LIT_RAW || type == ZSTD_LIT_RLE) {
        /* Size_Format 0/2: 5-bit size; 1: 12-bit; 3: 20-bit. */
        size_t hdr = sf == 1 ? 2 : sf == 3 ? 3 : 1;
        if (n < hdr)
            return 0;
        size_t regen = sf == 1 ? le(in, 2) >> 4 : sf == 3 ? le(in, 3) >> 4 : (size_t)(in[0] >> 3);
        if (regen > d->block_max)
            return 0;
        *nlit = regen;
        if (type == ZSTD_LIT_RAW) {
            if (regen > n - hdr)
                return 0;
            memcpy(d->lit, in + hdr, regen);
            return hdr + regen;
        }
        if (n < hdr + 1)
            return 0;
        memset(d->lit, in[hdr], regen);
        return hdr + 1;
    }
    /* Huffman: header of 3, 3, 4 or 5 bytes holding two equal-width sizes. */
    static const uint8_t hdr_bytes[4] = {3, 3, 4, 5}, size_bits[4] = {10, 10, 14, 18};
    size_t hdr = hdr_bytes[sf];
    if (n < hdr)
        return 0;
    uint64_t h = 0;
    for (size_t i = hdr; i-- > 0; )
        h = h << 8 | in[i];
    uint64_t mask = (1u << size_bits[sf]) - 1;
    size_t regen = (size_t)((h >> 4) & mask), comp = (size_t)((h >> (4 + size_bits[sf])) & mask);
    if (regen > d->block_max || comp > n - hdr || comp == 0)
        return 0;
    const uint8_t *p = in + hdr;
    size_t streams = comp;
    if (type == ZSTD_LIT_COMPRESSED) {
        size_t t = huf0_read_table(&d->huf, p, comp);
        if (!t)
            return 0;
        d->have_huf = 1;
        p += t;
        streams -= t;
    } else if (!d->have_huf) {
        return 0;  /* treeless literals need an earlier table */
    }
    if (huf0_decode_literals(&d->huf, p, streams, d->lit, regen, sf != 0))
        return 0;
    *nlit = regen;
    return hdr + comp;
}

/* ---- sequence tables ------------------------------------------------- */

static void rle_table(fse_dtable *t, unsigned symbol)
{
    t->log = 0;
    t->e[0] = (fse_dentry){(uint8_t)symbol, 0, 0};
}

static const unsigned max_code[3] = {ZSTD_LL_CODES, ZSTD_OF_CODES, ZSTD_ML_CODES};

/* Returns bytes consumed (0 for none), or -1 on error. */
static long seq_table(zstd_dctx *d, int type, unsigned mode, const uint8_t *in, size_t n)
{
    fse_dtable *t = &d->seq[type];
    switch (mode) {
    case ZSTD_MODE_PREDEFINED:
        fse_build_dtable(t, zstd_default_norm[type], zstd_default_nsyms[type], zstd_default_log[type]);
        break;
    case ZSTD_MODE_RLE:
        if (n < 1 || in[0] >= max_code[type])
            return -1;
        rle_table(t, in[0]);
        d->have_seq[type] = 1;
        return 1;
    case ZSTD_MODE_FSE: {
        fse_norm norm[ZSTD_ML_CODES];
        unsigned nsyms, log;
        size_t used = fse_read_ncount(in, n, norm, &nsyms, &log, zstd_max_log[type], max_code[type]);
        if (!used || fse_build_dtable(t, norm, nsyms, log))
            return -1;
        d->have_seq[type] = 1;
        return (long)used;
    }
    default:  /* repeat */
        if (!d->have_seq[type])
            return -1;
        return 0;
    }
    d->have_seq[type] = 1;
    return 0;
}

/* ---- sequences ------------------------------------------------------- */

static int copy_match(zstd_dctx *d, uint32_t offset, uint32_t len)
{
    /* References may reach into the dictionary content (floor 0). */
    return sq_buf_lzcopy(&d->w, 0, offset, len);
}

static int sequences(zstd_dctx *d, const uint8_t *in, size_t n, size_t nlit)
{
    if (n < 1)
        return SQ_ERR_DATA;
    size_t nseq = in[0], p = 1;
    if (nseq >= 128) {
        if (n < 2)
            return SQ_ERR_DATA;
        if (nseq < 255) {
            nseq = ((nseq - 128) << 8) + in[1];
            p = 2;
        } else {
            if (n < 3)
                return SQ_ERR_DATA;
            nseq = in[1] + ((size_t)in[2] << 8) + 0x7F00;
            p = 3;
        }
    }
    size_t lit_pos = 0, block_start = d->w.len;
    int err = SQ_OK;
    if (nseq > 0) {
        if (p >= n || (in[p] & 3))
            return SQ_ERR_DATA;  /* reserved bits */
        unsigned modes = in[p++];
        unsigned mode[3] = {modes >> 6, (modes >> 4) & 3, (modes >> 2) & 3};
        for (int k = 0; k < 3; k++) {
            long used = seq_table(d, k, mode[k], in + p, n - p);
            if (used < 0)
                return SQ_ERR_DATA;
            p += (size_t)used;
        }
        bitreader_rev br;
        if (brr_init(&br, in + p, n - p))
            return SQ_ERR_DATA;
        fse_dstate ll, of, ml;
        fse_dstate_init(&ll, &d->seq[ZSTD_LL], &br);
        fse_dstate_init(&of, &d->seq[ZSTD_OF], &br);
        fse_dstate_init(&ml, &d->seq[ZSTD_ML], &br);
        for (size_t i = 0; !err && i < nseq; i++) {
            unsigned ofc = fse_peek(&of), mlc = fse_peek(&ml), llc = fse_peek(&ll);
            /* Extra bits come in the order offset, match length, literal
             * length; then the states update as LL, ML, OF. */
            uint32_t offset_value = (1u << ofc) + (uint32_t)brr_read(&br, ofc);
            uint32_t mlen = zstd_ml_base[mlc] + (uint32_t)brr_read(&br, zstd_ml_bits[mlc]);
            uint32_t llen = zstd_ll_base[llc] + (uint32_t)brr_read(&br, zstd_ll_bits[llc]);
            if (i + 1 < nseq) {
                fse_update(&ll, &br);
                fse_update(&ml, &br);
                fse_update(&of, &br);
            }
            if (br.overrun || llen > nlit - lit_pos ||
                d->w.len - block_start + llen + mlen > d->block_max)
                return SQ_ERR_DATA;
            err = sq_buf_append(&d->w, d->lit + lit_pos, llen);
            lit_pos += llen;
            uint32_t offset = zstd_resolve_offset(d->rep, offset_value, llen);
            if (!err)
                err = offset ? copy_match(d, offset, mlen) : SQ_ERR_DATA;
        }
        if (!err && br.bits != 0)
            err = SQ_ERR_DATA;  /* the stream must be consumed exactly */
    } else if (p != n) {
        return SQ_ERR_DATA;
    }
    if (!err && d->w.len - block_start + (nlit - lit_pos) > d->block_max)
        err = SQ_ERR_DATA;
    if (!err)
        err = sq_buf_append(&d->w, d->lit + lit_pos, nlit - lit_pos);
    return err;
}

static int compressed_block(zstd_dctx *d, const uint8_t *in, size_t n)
{
    size_t nlit = 0, used = literals(d, in, n, &nlit);
    if (!used)
        return SQ_ERR_DATA;
    return sequences(d, in + used, n - used, nlit);
}

/* ---- dictionaries ---------------------------------------------------- */

/* Formatted dictionaries carry entropy tables and starting offsets;
 * anything else is raw content. Returns the dictionary ID (0 for raw) or
 * -1 if malformed. */
static long load_dict(zstd_dctx *d, const uint8_t *dict, size_t n)
{
    d->rep[0] = 1, d->rep[1] = 4, d->rep[2] = 8;
    d->have_huf = d->have_seq[0] = d->have_seq[1] = d->have_seq[2] = 0;
    if (n < 8 || le(dict, 4) != ZSTD_DICT_MAGIC)
        return sq_buf_append(&d->w, dict, n) ? -1 : 0;
    long id = (long)le(dict + 4, 4);
    size_t p = 8, k = huf0_read_table(&d->huf, dict + p, n - p);
    if (!k)
        return -1;
    d->have_huf = 1;
    p += k;
    static const int order[3] = {ZSTD_OF, ZSTD_ML, ZSTD_LL};
    for (int i = 0; i < 3; i++) {
        long used = seq_table(d, order[i], ZSTD_MODE_FSE, dict + p, n - p);
        if (used <= 0)
            return -1;
        p += (size_t)used;
    }
    if (n - p < 12)
        return -1;
    size_t content = n - p - 12;
    for (int i = 0; i < 3; i++) {
        d->rep[i] = le(dict + p + 4 * (size_t)i, 4);
        if (d->rep[i] == 0 || d->rep[i] > content)
            return -1;
    }
    p += 12;
    return sq_buf_append(&d->w, dict + p, content) ? -1 : id;
}

/* ---- frames ---------------------------------------------------------- */

static int frame(zstd_dctx *d, const uint8_t *in, size_t n, const uint8_t *dict, size_t dict_len,
                 sq_buf *out, size_t *used)
{
    if (n < 1)
        return SQ_ERR_DATA;
    unsigned fhd = in[0];
    unsigned fcs_flag = fhd >> 6, single = (fhd >> 5) & 1, checksum = (fhd >> 2) & 1;
    static const uint8_t did_bytes[4] = {0, 1, 2, 4}, fcs_bytes[4] = {0, 2, 4, 8};
    size_t did_len = did_bytes[fhd & 3];
    size_t fcs_len = fcs_flag == 0 ? single : fcs_bytes[fcs_flag];
    size_t hdr = 1 + !single + did_len + fcs_len;
    if (fhd & 0x08)
        return SQ_ERR_DATA;  /* reserved bit */
    if (n < hdr)
        return SQ_ERR_DATA;
    size_t p = 1;
    uint64_t window = 0;
    if (!single) {
        unsigned exp = in[p] >> 3, mant = in[p] & 7;
        uint64_t base = (uint64_t)1 << (10 + exp);
        window = base + base / 8 * mant;
        p++;
    }
    uint32_t dict_id = le(in + p, (int)did_len);
    p += did_len;
    uint64_t fcs = 0;
    for (size_t i = fcs_len; i-- > 0; )
        fcs = fcs << 8 | in[p + i];
    if (fcs_len == 2)
        fcs += 256;
    p += fcs_len;
    if (single)
        window = fcs;

    d->w.len = 0;
    long have_id = 0;
    if (dict && (have_id = load_dict(d, dict, dict_len)) < 0)
        return SQ_ERR_DATA;
    if (!dict)
        load_dict(d, NULL, 0);
    if (dict_id && (!dict || (have_id && (uint32_t)have_id != dict_id)))
        return dict ? SQ_ERR_DATA : SQ_ERR_UNSUPPORTED;  /* needs a dictionary */
    d->floor = d->w.len;
    d->block_max = window < ZSTD_BLOCK_MAX ? (size_t)window : ZSTD_BLOCK_MAX;

    for (int last = 0; !last;) {
        if (n - p < 3)
            return SQ_ERR_DATA;
        uint32_t bh = le(in + p, 3);
        p += 3;
        last = bh & 1;
        unsigned type = (bh >> 1) & 3;
        size_t size = bh >> 3;
        int err;
        if (type == ZSTD_BLOCK_RLE) {
            if (n - p < 1 || size > d->block_max)
                return SQ_ERR_DATA;
            err = sq_buf_reserve(&d->w, size);
            if (!err) {
                memset(d->w.data + d->w.len, in[p], size);
                d->w.len += size;
            }
            p += 1;
        } else {
            if (size > n - p || size > d->block_max)
                return SQ_ERR_DATA;
            if (type == ZSTD_BLOCK_RAW)
                err = sq_buf_append(&d->w, in + p, size);
            else if (type == ZSTD_BLOCK_COMPRESSED)
                err = compressed_block(d, in + p, size);
            else
                err = SQ_ERR_DATA;  /* reserved block type */
            p += size;
        }
        if (err)
            return err;
    }
    size_t produced = d->w.len - d->floor;
    if (fcs_len && fcs != produced)
        return SQ_ERR_DATA;
    if (checksum) {
        if (n - p < 4)
            return SQ_ERR_DATA;
        uint32_t want = (uint32_t)sq_xxh64(d->w.data + d->floor, produced, 0);
        if (le(in + p, 4) != want)
            return SQ_ERR_DATA;
        p += 4;
    }
    *used = p;
    return sq_buf_append(out, d->w.data + d->floor, produced);
}

int sq_zstd_decompress_dict(const uint8_t *in, size_t n, const uint8_t *dict, size_t dict_len,
                            sq_buf *out)
{
    zstd_dctx *d = malloc(sizeof *d);
    if (!d)
        return SQ_ERR_MEM;
    sq_buf_init(&d->w);
    int err = SQ_OK;
    size_t pos = 0;
    do {
        if (n - pos < 4) {
            err = SQ_ERR_DATA;
            break;
        }
        uint32_t magic = le(in + pos, 4);
        pos += 4;
        size_t used = 0;
        if (magic == ZSTD_MAGIC) {
            err = frame(d, in + pos, n - pos, dict, dict_len, out, &used);
        } else if ((magic & ZSTD_SKIPPABLE_MASK) == ZSTD_SKIPPABLE) {
            if (n - pos < 4 || le(in + pos, 4) > n - pos - 4)
                err = SQ_ERR_DATA;
            else
                used = 4 + le(in + pos, 4);
        } else {
            err = SQ_ERR_DATA;
        }
        pos += used;
    } while (!err && pos < n);
    sq_buf_free(&d->w);
    free(d);
    return err;
}

int sq_zstd_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    return sq_zstd_decompress_dict(in, n, NULL, 0, out);
}
