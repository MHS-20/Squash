/* .lzma, LZMA2 and .xz: reference vectors, error paths, and the encoder's
 * corner cases checked against the real xz when it is installed. */
#include "lzma.h"
#include "test.h"

typedef int (*decoder)(const uint8_t *, size_t, sq_buf *);

static int decodes_to(decoder f, const uint8_t *p, size_t n, const char *want, int err_want)
{
    sq_buf b;
    sq_buf_init(&b);
    int err = f(p, n, &b);
    int ok = want ? !err && same_bytes(b.data, b.len, want, strlen(want)) : err == err_want;
    sq_buf_free(&b);
    return ok;
}

static void official(const char *what, const sq_buf *z, const uint8_t *want, size_t wn,
                     const char *cmd)
{
    int r = tool_decodes("xz", cmd, z->data, z->len, want, wn);
    if (r < 0)
        printf("lzma           SKIPPED official check of %s (xz not found)\n", what);
    else
        CHECK(r == 1);
}

int main(void)
{
    /* `printf hello | xz --format=lzma`: unknown size, so an end marker. */
    static const uint8_t alone[] = {0x5d, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0x00, 0x34, 0x19, 0x49, 0xee, 0x8e, 0x68,
                                    0x21, 0xff, 0xff, 0xff, 0xb9, 0xe0, 0x00, 0x00};
    CHECK(decodes_to(sq_lzma_decompress, alone, sizeof alone, "hello", 0));
    uint8_t bad[sizeof alone];
    memcpy(bad, alone, sizeof bad);
    bad[0] = 225;  /* properties out of range */
    CHECK(decodes_to(sq_lzma_decompress, bad, sizeof bad, NULL, SQ_ERR_DATA));
    memcpy(bad, alone, sizeof bad);
    bad[5] = 3, memset(bad + 6, 0, 7);  /* claims 3 bytes: stops early, coder unfinished */
    CHECK(decodes_to(sq_lzma_decompress, bad, sizeof bad, NULL, SQ_ERR_DATA));
    CHECK(decodes_to(sq_lzma_decompress, alone, sizeof alone - 3, NULL, SQ_ERR_DATA));

    /* `printf hello | xz --check=crc32`: one stored LZMA2 chunk. Flipping
     * a bit anywhere must be caught. */
    static const uint8_t xz[] = {
        0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x01, 0x69, 0x22, 0xde, 0x36, 0x04, 0xc0,
        0x09, 0x05, 0x21, 0x01, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xbf, 0x79, 0x25, 0x67, 0x01, 0x00, 0x04, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00,
        0x00, 0x00, 0x86, 0xa6, 0x10, 0x36, 0x00, 0x01, 0x21, 0x05, 0x47, 0x54, 0x73, 0xdc,
        0x90, 0x42, 0x99, 0x0d, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x59, 0x5a};
    CHECK(decodes_to(sq_xz_decompress, xz, sizeof xz, "hello", 0));
    uint8_t x2[sizeof xz + 4 + sizeof xz] = {0};  /* stream, 4 bytes padding, stream */
    memcpy(x2, xz, sizeof xz);
    memcpy(x2 + sizeof xz + 4, xz, sizeof xz);
    CHECK(decodes_to(sq_xz_decompress, x2, sizeof x2, "hellohello", 0));
    CHECK(decodes_to(sq_xz_decompress, x2, sizeof xz + 2, NULL, SQ_ERR_DATA));  /* bad padding */
    uint8_t xb[sizeof xz];
    size_t flips[] = {7, 20, 36, 44, 49, 60, 66};  /* flags, header, data, check, index, footer */
    for (size_t i = 0; i < sizeof flips / sizeof flips[0]; i++) {
        memcpy(xb, xz, sizeof xb);
        xb[flips[i]] ^= 0x01;
        sq_buf o;
        sq_buf_init(&o);
        CHECK(sq_xz_decompress(xb, sizeof xb, &o) != 0);  /* every field is CRC-protected */
        sq_buf_free(&o);
    }
    /* `xz --x86 --lzma2`: a BCJ filter chain, recognised but unsupported. */
    static const uint8_t bcj[] = {
        0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x00, 0xff, 0x12, 0xd9, 0x41, 0x04, 0xc1,
        0x0f, 0x0b, 0x04, 0x00, 0x21, 0x01, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xd7, 0x23, 0x66, 0xba, 0x01, 0x00, 0x0a, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x68,
        0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00, 0x01, 0x23, 0x0b, 0xc2, 0x1b, 0xfd, 0x09,
        0x06, 0x72, 0x9e, 0x7a, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59, 0x5a};
    CHECK(decodes_to(sq_xz_decompress, bcj, sizeof bcj, NULL, SQ_ERR_UNSUPPORTED));

    /* Encoder corner cases: an incompressible start (stored first chunk,
     * so the next LZMA chunk must carry properties), long runs (rep
     * matches, maximum lengths), and data spanning several chunks. */
    static uint8_t data[600000];
    fill_random(data, 150000, 61);
    for (size_t i = 150000; i < sizeof data; i++)
        data[i] = (i % 5000 < 3000) ? (uint8_t)"abcabcabd"[i % 9] : data[i - 150000];
    sq_buf z, d;
    sq_buf_init(&z);
    sq_buf_init(&d);
    CHECK(sq_xz_compress(data, sizeof data, 1, &z) == 0);
    CHECK(sq_xz_decompress(z.data, z.len, &d) == 0 && same_bytes(d.data, d.len, data, sizeof data));
    official("xz", &z, data, sizeof data, "xz -dc");
    z.len = d.len = 0;
    CHECK(sq_lzma_compress(data, sizeof data, 6, &z) == 0);
    CHECK(sq_lzma_decompress(z.data, z.len, &d) == 0 && same_bytes(d.data, d.len, data, sizeof data));
    official(".lzma", &z, data, sizeof data, "xz -dc --format=lzma");

    /* An end marker written after known-size data is not required, but an
     * unknown-size stream without one is an error: encode with the marker
     * by hand and check both behaviours. */
    lzma_encoder e;
    CHECK(lzma_encoder_init(&e, data + 150000, 1000, 6) == 0);
    z.len = 0;
    const uint8_t hdr[13] = {0x5d, 0, 0x10, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    sq_buf_append(&z, hdr, sizeof hdr);
    rc_encoder rc;
    rc_enc_init(&rc, &z);
    while (e.pos < e.n)
        lzma_encode_step(&e, &rc);
    size_t before_marker = z.len;
    lzma_encode_end_marker(&e, &rc);
    rc_enc_flush(&rc);
    lzma_encoder_free(&e);
    d.len = 0;
    CHECK(sq_lzma_decompress(z.data, z.len, &d) == 0 && same_bytes(d.data, d.len, data + 150000, 1000));
    official("end marker", &z, data + 150000, 1000, "xz -dc --format=lzma");
    d.len = 0;
    CHECK(sq_lzma_decompress(z.data, before_marker, &d) != 0);

    sq_buf_free(&z);
    sq_buf_free(&d);
    TEST_DONE("lzma");
}
