/* The .xz container (xz-file-format.txt): streams of
 *   header (magic, flags, CRC32) | blocks | index | footer
 * Each block has its own header (filter chain, optional sizes, CRC32),
 * the filtered data, zero padding to 4 bytes, and the integrity check of
 * the uncompressed data. The index lists every block's sizes so the
 * stream can be verified (or seeked) from the end. */
#include <stdlib.h>
#include <string.h>

#include "checksum.h"
#include "lzma.h"

static const uint8_t header_magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
static const uint8_t footer_magic[2] = {'Y', 'Z'};

enum { CHECK_NONE = 0, CHECK_CRC32 = 1, CHECK_CRC64 = 4, CHECK_SHA256 = 10 };
#define FILTER_LZMA2 0x21

static size_t check_size(unsigned type)
{
    /* Sizes for every ID, including reserved ones, so unknown checks can
     * still be skipped over: 0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, ... */
    return type == 0 ? 0 : (size_t)4 << ((type - 1) / 3);
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* xz's variable-length integers: 7 bits per byte, LSB group first, at
 * most 9 bytes, no redundant trailing zero byte. Returns bytes used, or
 * 0 if malformed. */
static size_t get_varint(const uint8_t *p, size_t n, uint64_t *v)
{
    *v = 0;
    for (size_t i = 0; i < n && i < 9; i++) {
        *v |= (uint64_t)(p[i] & 0x7F) << (7 * i);
        if (!(p[i] & 0x80))
            return (i > 0 && p[i] == 0) ? 0 : i + 1;
    }
    return 0;
}

static int put_varint(sq_buf *out, uint64_t v)
{
    int err;
    for (; v >= 0x80; v >>= 7)
        if ((err = sq_buf_push(out, (uint8_t)(v | 0x80))))
            return err;
    return sq_buf_push(out, (uint8_t)v);
}

static int pad4(sq_buf *out, size_t since)
{
    int err = SQ_OK;
    while (!err && (out->len - since) % 4)
        err = sq_buf_push(out, 0);
    return err;
}

static void compute_check(unsigned type, const uint8_t *p, size_t n, uint8_t *dst)
{
    if (type == CHECK_CRC32) {
        uint32_t c = sq_crc32(0, p, n);
        for (int i = 0; i < 4; i++)
            dst[i] = (uint8_t)(c >> (8 * i));
    } else if (type == CHECK_CRC64) {
        uint64_t c = sq_crc64(0, p, n);
        for (int i = 0; i < 8; i++)
            dst[i] = (uint8_t)(c >> (8 * i));
    } else if (type == CHECK_SHA256) {
        sq_sha256(p, n, dst);
    }
}

/* Dictionary size as one byte: 2 or 3 times a power of two. */
static uint8_t dict_byte(uint32_t dict)
{
    uint8_t b = 0;
    while (b < 40 && ((uint64_t)(2 | (b & 1)) << (b / 2 + 11)) < dict)
        b++;
    return b;
}

int sq_xz_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    const unsigned check = CHECK_CRC64;
    uint8_t flags[2] = {0, check};
    int err = sq_buf_append(out, header_magic, 6);
    if (!err)
        err = sq_buf_append(out, flags, 2);
    if (!err)
        err = sq_buf_put_le(out, sq_crc32(0, flags, 2), 4);

    /* One block holding everything (no block for empty input). */
    size_t blocks = 0, unpadded = 0;
    if (!err && n > 0) {
        size_t block_start = out->len;
        sq_buf data;
        sq_buf_init(&data);
        uint32_t dict;
        err = lzma2_compress(in, n, level, &data, &dict);
        /* Block header: size byte, flags (one filter, no sizes), filter
         * flags (ID, property size, dictionary byte), padding, CRC32. */
        uint8_t hdr[12] = {0, 0x00, FILTER_LZMA2, 1, dict_byte(dict)};
        hdr[0] = sizeof hdr / 4 - 1;
        uint32_t crc = sq_crc32(0, hdr, 8);
        for (int i = 0; i < 4; i++)
            hdr[8 + i] = (uint8_t)(crc >> (8 * i));
        if (!err)
            err = sq_buf_append(out, hdr, sizeof hdr);
        if (!err)
            err = sq_buf_append(out, data.data, data.len);
        unpadded = sizeof hdr + data.len + check_size(check);
        sq_buf_free(&data);
        if (!err)
            err = pad4(out, block_start);
        uint8_t sum[32];
        compute_check(check, in, n, sum);
        if (!err)
            err = sq_buf_append(out, sum, check_size(check));
        blocks = 1;
    }

    /* Index: indicator, record count, (unpadded, uncompressed) records. */
    size_t index_start = out->len;
    if (!err)
        err = sq_buf_push(out, 0x00);
    if (!err)
        err = put_varint(out, blocks);
    if (!err && blocks) {
        err = put_varint(out, unpadded);
        if (!err)
            err = put_varint(out, n);
    }
    if (!err)
        err = pad4(out, index_start);
    if (!err)
        err = sq_buf_put_le(out, sq_crc32(0, out->data + index_start, out->len - index_start), 4);
    if (err)
        return err;

    uint8_t footer[12];
    uint32_t backward = (uint32_t)((out->len - index_start) / 4 - 1);
    for (int i = 0; i < 4; i++)
        footer[4 + i] = (uint8_t)(backward >> (8 * i));
    footer[8] = flags[0];
    footer[9] = flags[1];
    uint32_t crc = sq_crc32(0, footer + 4, 6);
    for (int i = 0; i < 4; i++)
        footer[i] = (uint8_t)(crc >> (8 * i));
    memcpy(footer + 10, footer_magic, 2);
    return sq_buf_append(out, footer, sizeof footer);
}

/* ---- decoder --------------------------------------------------------- */

typedef struct {
    uint64_t unpadded, uncompressed;
} record;

/* Block sizes as observed, kept to validate the index against. */
typedef struct {
    record *r;
    size_t n, cap;
} records;

static int add_record(records *rs, uint64_t unpadded, uint64_t uncompressed)
{
    if (rs->n == rs->cap) {
        size_t cap = rs->cap ? 2 * rs->cap : 16;
        record *r = realloc(rs->r, cap * sizeof *r);
        if (!r)
            return SQ_ERR_MEM;
        rs->r = r;
        rs->cap = cap;
    }
    rs->r[rs->n++] = (record){unpadded, uncompressed};
    return SQ_OK;
}

static int block(const uint8_t *in, size_t n, unsigned check, sq_buf *out, size_t *used,
                 records *rs)
{
    size_t hsize = ((size_t)in[0] + 1) * 4;
    if (hsize > n || le32(in + hsize - 4) != sq_crc32(0, in, hsize - 4))
        return SQ_ERR_DATA;
    unsigned bflags = in[1];
    if (bflags & 0x3C)
        return SQ_ERR_DATA;  /* reserved bits */
    size_t p = 2, end = hsize - 4, k;
    uint64_t csize = UINT64_MAX, usize = UINT64_MAX;
    if ((bflags & 0x40) && (!(k = get_varint(in + p, end - p, &csize)) || csize == 0))
        return SQ_ERR_DATA;
    p += (bflags & 0x40) ? k : 0;
    if ((bflags & 0x80) && !(k = get_varint(in + p, end - p, &usize)))
        return SQ_ERR_DATA;
    p += (bflags & 0x80) ? k : 0;
    unsigned nfilters = (bflags & 3) + 1;
    uint64_t id = 0, psize = 0;
    for (unsigned f = 0; f < nfilters; f++) {
        if (!(k = get_varint(in + p, end - p, &id)))
            return SQ_ERR_DATA;
        p += k;
        if (!(k = get_varint(in + p, end - p, &psize)) || psize > end - p - k)
            return SQ_ERR_DATA;
        p += k + psize;
    }
    if (nfilters != 1 || id != FILTER_LZMA2)
        return SQ_ERR_UNSUPPORTED;  /* BCJ, delta and other filter chains */
    if (psize != 1 || (in[p - 1] & 0xC0) || in[p - 1] > 40)
        return SQ_ERR_DATA;
    for (; p < end; p++)
        if (in[p])
            return SQ_ERR_DATA;  /* header padding must be zero */

    size_t start = out->len, clen;
    int err = lzma2_decompress(in + hsize, n - hsize, out, &clen);
    if (err)
        return err;
    size_t ulen = out->len - start;
    if ((csize != UINT64_MAX && csize != clen) || (usize != UINT64_MAX && usize != ulen))
        return SQ_ERR_DATA;
    p = hsize + clen;
    while (p % 4) {
        if (p >= n || in[p++])
            return SQ_ERR_DATA;  /* block padding */
    }
    size_t cs = check_size(check);
    if (cs > n - p)
        return SQ_ERR_DATA;
    uint8_t sum[32];
    compute_check(check, out->data + start, ulen, sum);
    if (check != CHECK_NONE && memcmp(sum, in + p, cs) != 0)
        return SQ_ERR_DATA;
    *used = p + cs;
    return add_record(rs, hsize + clen + cs, ulen);
}

static int index_and_footer(const uint8_t *in, size_t n, const uint8_t *flags, const records *rs,
                            size_t *used)
{
    size_t p = 1, k;  /* in[0] is the 0x00 indicator */
    uint64_t count;
    if (!(k = get_varint(in + p, n - p, &count)) || count != rs->n)
        return SQ_ERR_DATA;
    p += k;
    for (size_t i = 0; i < rs->n; i++) {
        uint64_t unpadded, uncompressed;
        if (!(k = get_varint(in + p, n - p, &unpadded)))
            return SQ_ERR_DATA;
        p += k;
        if (!(k = get_varint(in + p, n - p, &uncompressed)))
            return SQ_ERR_DATA;
        p += k;
        if (unpadded != rs->r[i].unpadded || uncompressed != rs->r[i].uncompressed)
            return SQ_ERR_DATA;
    }
    while (p % 4) {
        if (p >= n || in[p++])
            return SQ_ERR_DATA;
    }
    if (n - p < 4 + 12 || le32(in + p) != sq_crc32(0, in, p))
        return SQ_ERR_DATA;
    size_t index_size = p + 4;
    const uint8_t *f = in + index_size;
    if (le32(f) != sq_crc32(0, f + 4, 6) || ((size_t)le32(f + 4) + 1) * 4 != index_size ||
        memcmp(f + 8, flags, 2) != 0 || memcmp(f + 10, footer_magic, 2) != 0)
        return SQ_ERR_DATA;
    *used = index_size + 12;
    return SQ_OK;
}

static int stream(const uint8_t *in, size_t n, sq_buf *out, size_t *used)
{
    if (n < 12 || memcmp(in, header_magic, 6) != 0)
        return SQ_ERR_DATA;
    const uint8_t *flags = in + 6;
    if (le32(in + 8) != sq_crc32(0, flags, 2))
        return SQ_ERR_DATA;
    if (flags[0] != 0 || (flags[1] & 0xF0))
        return SQ_ERR_DATA;  /* reserved bits */
    unsigned check = flags[1];
    if (check != CHECK_NONE && check != CHECK_CRC32 && check != CHECK_CRC64 && check != CHECK_SHA256)
        return SQ_ERR_UNSUPPORTED;
    records rs = {NULL, 0, 0};
    size_t p = 12, k;
    int err = SQ_OK;
    while (!err) {
        if (p >= n) {
            err = SQ_ERR_DATA;
            break;
        }
        if (in[p] == 0x00) {
            err = index_and_footer(in + p, n - p, flags, &rs, &k);
            if (!err)
                *used = p + k;
            break;
        }
        err = block(in + p, n - p, check, out, &k, &rs);
        p += k;
    }
    free(rs.r);
    return err;
}

int sq_xz_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    size_t pos = 0;
    do {
        size_t used;
        int err = stream(in + pos, n - pos, out, &used);
        if (err)
            return err;
        pos += used;
        /* Stream padding: zero bytes in multiples of four, then maybe
         * another stream. */
        size_t pad = pos;
        while (pos < n && in[pos] == 0)
            pos++;
        if ((pos - pad) % 4)
            return SQ_ERR_DATA;
    } while (pos < n);
    return SQ_OK;
}
