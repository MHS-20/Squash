/* DEFLATE, zlib and gzip against hand-made and reference-produced streams. */
#include "bitio.h"
#include "deflate.h"
#include "huffman.h"
#include "test.h"

static int dec(int (*f)(const uint8_t *, size_t, sq_buf *), const void *p, size_t n,
               const char *want)
{
    sq_buf b;
    sq_buf_init(&b);
    int err = f(p, n, &b);
    int ok = want ? (!err && same_bytes(b.data, b.len, want, strlen(want))) : err != 0;
    sq_buf_free(&b);
    return ok;
}

int main(void)
{
    /* Python zlib.compress(b"hello"): one fixed-Huffman block. */
    static const uint8_t zhello[] = {0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07,
                                     0x00, 0x06, 0x2c, 0x02, 0x15};
    CHECK(dec(sq_zlib_decompress, zhello, sizeof zhello, "hello"));
    /* Same with a wrong Adler-32 and with a broken header checksum. */
    uint8_t bad[sizeof zhello];
    memcpy(bad, zhello, sizeof bad);
    bad[12] ^= 1;
    CHECK(dec(sq_zlib_decompress, bad, sizeof bad, NULL));
    memcpy(bad, zhello, sizeof bad);
    bad[1] ^= 1;
    CHECK(dec(sq_zlib_decompress, bad, sizeof bad, NULL));

    /* `printf hello | gzip -n`: stored-name-free member. */
    static const uint8_t ghello[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x03, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07,
                                     0x00, 0x86, 0xa6, 0x10, 0x36, 0x05, 0x00, 0x00, 0x00};
    CHECK(dec(sq_gzip_decompress, ghello, sizeof ghello, "hello"));
    uint8_t two[2 * sizeof ghello];  /* concatenated members */
    memcpy(two, ghello, sizeof ghello);
    memcpy(two + sizeof ghello, ghello, sizeof ghello);
    CHECK(dec(sq_gzip_decompress, two, sizeof two, "hellohello"));

    /* Stored block: BFINAL=1, BTYPE=00, LEN=5, NLEN=~5. */
    static const uint8_t stored[] = {0x01, 0x05, 0x00, 0xfa, 0xff, 'h', 'e', 'l', 'l', 'o'};
    CHECK(dec(sq_deflate_decompress, stored, sizeof stored, "hello"));
    static const uint8_t stored_bad[] = {0x01, 0x05, 0x00, 0xfa, 0xfe, 'h', 'e', 'l', 'l', 'o'};
    CHECK(dec(sq_deflate_decompress, stored_bad, sizeof stored_bad, NULL));
    CHECK(dec(sq_deflate_decompress, stored, sizeof stored - 1, NULL));  /* short */
    static const uint8_t reserved[] = {0x07};  /* BTYPE=11 */
    CHECK(dec(sq_deflate_decompress, reserved, 1, NULL));

    /* Fixed block whose first symbol is a match: distance reaches before
     * the start of the output. Lengths/distances use fixed codes. */
    uint8_t ll[288], dl[30];
    uint32_t lc[288], dc[30];
    deflate_fixed_lengths(ll, dl);
    huff_codes(ll, 288, lc);
    huff_codes(dl, 30, dc);
    sq_buf b;
    sq_buf_init(&b);
    bitwriter_lsb bw;
    bwl_init(&bw, &b);
    bwl_put(&bw, 1, 1);
    bwl_put(&bw, 1, 2);
    bwl_put(&bw, huff_reverse(lc[257], 7), 7);  /* length 3 */
    bwl_put(&bw, huff_reverse(dc[0], 5), 5);    /* distance 1 */
    bwl_put(&bw, huff_reverse(lc[256], 7), 7);
    bwl_flush(&bw);
    CHECK(dec(sq_deflate_decompress, b.data, b.len, NULL));
    /* The same match after one literal is a valid run: "aaaa". */
    b.len = 0;
    bwl_init(&bw, &b);
    bwl_put(&bw, 1, 1);
    bwl_put(&bw, 1, 2);
    bwl_put(&bw, huff_reverse(lc['a'], 8), 8);
    bwl_put(&bw, huff_reverse(lc[257], 7), 7);
    bwl_put(&bw, huff_reverse(dc[0], 5), 5);
    bwl_put(&bw, huff_reverse(lc[256], 7), 7);
    bwl_flush(&bw);
    CHECK(dec(sq_deflate_decompress, b.data, b.len, "aaaa"));
    /* Reserved length symbol 286. */
    b.len = 0;
    bwl_init(&bw, &b);
    bwl_put(&bw, 1, 1);
    bwl_put(&bw, 1, 2);
    bwl_put(&bw, huff_reverse(lc[286], 8), 8);
    bwl_flush(&bw);
    CHECK(dec(sq_deflate_decompress, b.data, b.len, NULL));

    /* Empty input compresses to a valid, tiny stream. */
    b.len = 0;
    CHECK(sq_deflate_compress(NULL, 0, 6, &b) == 0);
    CHECK(b.len <= 2);
    CHECK(dec(sq_deflate_decompress, b.data, b.len, ""));

    /* Incompressible data falls back to stored blocks: bounded overhead. */
    static uint8_t rnd[200000];
    fill_random(rnd, sizeof rnd, 4);
    b.len = 0;
    CHECK(sq_deflate_compress(rnd, sizeof rnd, 6, &b) == 0);
    CHECK(b.len < sizeof rnd + sizeof rnd / 1000 + 64);
    sq_buf_free(&b);
    TEST_DONE("deflate");
}
