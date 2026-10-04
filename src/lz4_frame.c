/* LZ4 frame format (lz4_Frame_format.md): magic, frame descriptor, data
 * blocks, end mark, optional content checksum. The decoder also accepts
 * skippable frames, the legacy frame format, and several frames in a row,
 * like the `lz4` tool. */
#include "checksum.h"
#include "lz4.h"

#define LZ4F_MAGIC 0x184D2204u
#define LZ4F_LEGACY_MAGIC 0x184C2102u
#define LZ4F_LEGACY_BLOCK (8u << 20)
#define LZ4F_SKIPPABLE_MASK 0xFFFFFFF0u
#define LZ4F_SKIPPABLE 0x184D2A50u

enum { FLG_DICT_ID = 0x01, FLG_C_CHECKSUM = 0x04, FLG_C_SIZE = 0x08,
       FLG_B_CHECKSUM = 0x10, FLG_B_INDEP = 0x20 };

/* Block maximum sizes for BD codes 4..7. */
static size_t block_max(unsigned code) { return (size_t)1 << (8 + 2 * code); }

/* 4 MB blocks, the lz4 tool's default: larger blocks give the 64 KB window
 * more context and pay the block header less often. */
#define ENC_BD 7

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

int sq_lz4_compress(const uint8_t *in, size_t n, int level, sq_buf *out)
{
    if (level < 0 || level > 9)
        return SQ_ERR_ARG;
    /* Version 01, independent blocks, content size, content checksum. */
    uint8_t desc[11] = {0x40 | FLG_B_INDEP | FLG_C_SIZE | FLG_C_CHECKSUM, ENC_BD << 4};
    for (int i = 0; i < 8; i++)
        desc[2 + i] = (uint8_t)((uint64_t)n >> (8 * i));
    desc[10] = (uint8_t)(sq_xxh32(desc, 10, 0) >> 8);
    int err = sq_buf_put_le(out, LZ4F_MAGIC, 4);
    if (!err)
        err = sq_buf_append(out, desc, sizeof desc);

    size_t bmax = block_max(ENC_BD);
    for (size_t pos = 0; !err && pos < n; pos += bmax) {
        size_t len = n - pos < bmax ? n - pos : bmax;
        size_t at = out->len;
        err = sq_buf_put_le(out, 0, 4);  /* size, patched below */
        if (!err)
            err = lz4_block_compress(in + pos, len, level, out);
        if (err)
            break;
        size_t clen = out->len - at - 4;
        uint32_t field = (uint32_t)clen;
        if (clen >= len) {
            /* Incompressible: the spec requires storing it raw instead. */
            out->len = at + 4;
            err = sq_buf_append(out, in + pos, len);
            field = (uint32_t)len | 0x80000000u;
        }
        for (int i = 0; i < 4; i++)
            out->data[at + i] = (uint8_t)(field >> (8 * i));
    }
    if (!err)
        err = sq_buf_put_le(out, 0, 4);  /* EndMark */
    if (!err)
        err = sq_buf_put_le(out, sq_xxh32(in, n, 0), 4);
    return err;
}

/* One LZ4 frame starting after its magic number. */
static int frame(const uint8_t *in, size_t n, sq_buf *out, size_t *used)
{
    if (n < 3)
        return SQ_ERR_DATA;
    unsigned flg = in[0], bd = in[1];
    if ((flg >> 6) != 1 || (flg & 0x02) || (bd & 0x8F) || ((bd >> 4) & 7) < 4)
        return SQ_ERR_DATA;  /* version, reserved bits, block size code */
    size_t dlen = 2 + (flg & FLG_C_SIZE ? 8 : 0) + (flg & FLG_DICT_ID ? 4 : 0);
    if (n < dlen + 1)
        return SQ_ERR_DATA;
    if (in[dlen] != (uint8_t)(sq_xxh32(in, dlen, 0) >> 8))
        return SQ_ERR_DATA;
    if (flg & FLG_DICT_ID)
        return SQ_ERR_UNSUPPORTED;
    size_t bmax = block_max((bd >> 4) & 7), pos = dlen + 1;
    size_t start = out->len;

    for (;;) {
        if (n - pos < 4)
            return SQ_ERR_DATA;
        uint32_t field = le32(in + pos);
        pos += 4;
        if (field == 0)
            break;  /* EndMark */
        size_t len = field & 0x7FFFFFFFu;
        size_t extra = flg & FLG_B_CHECKSUM ? 4 : 0;
        if (len > bmax || len + extra > n - pos)
            return SQ_ERR_DATA;
        if (extra && le32(in + pos + len) != sq_xxh32(in + pos, len, 0))
            return SQ_ERR_DATA;
        int err;
        if (field & 0x80000000u) {
            err = sq_buf_append(out, in + pos, len);
        } else {
            /* Linked blocks may refer back into earlier blocks (the 16-bit
             * offset itself caps how far); independent ones may not. */
            size_t floor = flg & FLG_B_INDEP ? out->len : start;
            err = lz4_block_decompress(in + pos, len, out, floor, bmax);
        }
        if (err)
            return err;
        pos += len + extra;
    }
    if ((flg & FLG_C_SIZE) && le64(in + 2) != out->len - start)
        return SQ_ERR_DATA;
    if (flg & FLG_C_CHECKSUM) {
        if (n - pos < 4)
            return SQ_ERR_DATA;
        if (le32(in + pos) != sq_xxh32(out->data + start, out->len - start, 0))
            return SQ_ERR_DATA;
        pos += 4;
    }
    *used = pos;
    return SQ_OK;
}

/* Legacy frame: 8 MB independent blocks, each prefixed by its compressed
 * size, ending at end of input or at the next frame's magic number. */
static int legacy_frame(const uint8_t *in, size_t n, sq_buf *out, size_t *used)
{
    size_t pos = 0;
    while (n - pos >= 4) {
        uint32_t len = le32(in + pos);
        if (len == LZ4F_MAGIC || len == LZ4F_LEGACY_MAGIC ||
            (len & LZ4F_SKIPPABLE_MASK) == LZ4F_SKIPPABLE)
            break;
        pos += 4;
        if (len > n - pos || len > LZ4F_LEGACY_BLOCK)
            return SQ_ERR_DATA;
        int err = lz4_block_decompress(in + pos, len, out, out->len, LZ4F_LEGACY_BLOCK);
        if (err)
            return err;
        pos += len;
    }
    if (pos != n && n - pos < 4)
        return SQ_ERR_DATA;
    *used = pos;
    return SQ_OK;
}

int sq_lz4_decompress(const uint8_t *in, size_t n, sq_buf *out)
{
    size_t pos = 0;
    do {
        if (n - pos < 4)
            return SQ_ERR_DATA;
        uint32_t magic = le32(in + pos);
        pos += 4;
        size_t used;
        int err;
        if (magic == LZ4F_MAGIC) {
            err = frame(in + pos, n - pos, out, &used);
        } else if (magic == LZ4F_LEGACY_MAGIC) {
            err = legacy_frame(in + pos, n - pos, out, &used);
        } else if ((magic & LZ4F_SKIPPABLE_MASK) == LZ4F_SKIPPABLE) {
            if (n - pos < 4 || le32(in + pos) > n - pos - 4)
                return SQ_ERR_DATA;
            used = 4 + le32(in + pos);
            err = SQ_OK;
        } else {
            return SQ_ERR_DATA;
        }
        if (err)
            return err;
        pos += used;
    } while (pos < n);
    return SQ_OK;
}
