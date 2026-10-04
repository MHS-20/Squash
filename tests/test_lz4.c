#include "lz4.h"
#include "test.h"

static int frame_is(const uint8_t *p, size_t n, const char *want)
{
    sq_buf b;
    sq_buf_init(&b);
    int err = sq_lz4_decompress(p, n, &b);
    int ok = want ? !err && same_bytes(b.data, b.len, want, strlen(want)) : err != 0;
    sq_buf_free(&b);
    return ok;
}

static int block_is(const uint8_t *p, size_t n, const char *want)
{
    sq_buf b;
    sq_buf_init(&b);
    int err = lz4_block_decompress(p, n, &b, 0, 1 << 16);
    int ok = want ? !err && same_bytes(b.data, b.len, want, strlen(want)) : err != 0;
    sq_buf_free(&b);
    return ok;
}

int main(void)
{
    /* `printf 'hello hello hello hello\n' | lz4 -c --no-frame-crc`. */
    static const uint8_t ref[] = {0x04, 0x22, 0x4d, 0x18, 0x60, 0x40, 0x82, 0x0f, 0x00, 0x00,
                                  0x00, 0x69, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x06, 0x00,
                                  0x50, 0x65, 0x6c, 0x6c, 0x6f, 0x0a, 0x00, 0x00, 0x00, 0x00};
    CHECK(frame_is(ref, sizeof ref, "hello hello hello hello\n"));
    /* `printf '' | lz4 -c`: header, EndMark, checksum of nothing. */
    static const uint8_t empty[] = {0x04, 0x22, 0x4d, 0x18, 0x64, 0x40, 0xa7, 0x00,
                                    0x00, 0x00, 0x00, 0x05, 0x5d, 0xcc, 0x02};
    CHECK(frame_is(empty, sizeof empty, ""));
    uint8_t bad[sizeof ref];
    memcpy(bad, ref, sizeof bad);
    bad[6] ^= 1;  /* header checksum */
    CHECK(frame_is(bad, sizeof bad, NULL));
    memcpy(bad, ref, sizeof bad);
    bad[5] = 0x30;  /* block size code 3 is not defined */
    CHECK(frame_is(bad, sizeof bad, NULL));
    uint8_t two[2 * sizeof ref];  /* concatenated frames */
    memcpy(two, ref, sizeof ref);
    memcpy(two + sizeof ref, ref, sizeof ref);
    CHECK(frame_is(two, sizeof two, "hello hello hello hello\nhello hello hello hello\n"));
    /* Skippable frame (magic 0x184D2A5F, 3 bytes of payload) then a frame. */
    uint8_t skip[7 + 4 + sizeof empty] = {0x5f, 0x2a, 0x4d, 0x18, 3, 0, 0, 0, 1, 2, 3};
    memcpy(skip + 11, empty, sizeof empty);
    CHECK(frame_is(skip, sizeof skip, ""));

    /* Blocks: a lone zero token is the empty block; a match of offset 1
     * repeats the last byte; offset 0 and offsets before the start fail. */
    CHECK(block_is((const uint8_t *)"\x00", 1, ""));
    CHECK(block_is((const uint8_t *)"\x10" "a" "\x01\x00" "\x00", 5, "aaaaa"));
    CHECK(block_is((const uint8_t *)"\x10" "a" "\x00\x00" "\x00", 5, NULL));
    CHECK(block_is((const uint8_t *)"\x10" "a" "\x02\x00" "\x00", 5, NULL));
    CHECK(block_is((const uint8_t *)"", 0, NULL));
    CHECK(block_is((const uint8_t *)"\x50" "ab", 3, NULL));  /* literals cut short */
    CHECK(block_is((const uint8_t *)"\x10" "a" "\x01", 3, NULL));  /* offset cut short */
    /* 15 + 255 + 3 literal bytes via the length continuation. */
    static uint8_t longlit[1 + 2 + 273];
    longlit[0] = 0xF0, longlit[1] = 255, longlit[2] = 3;
    memset(longlit + 3, 'x', 273);
    sq_buf b;
    sq_buf_init(&b);
    CHECK(lz4_block_decompress(longlit, sizeof longlit, &b, 0, 1 << 16) == 0 && b.len == 273);
    b.len = 0;
    CHECK(lz4_block_decompress(longlit, sizeof longlit, &b, 0, 100) != 0);  /* over budget */

    /* Our blocks keep the end-of-block rules: the last 5 bytes are literals
     * and short inputs are stored as a single literal run. */
    static uint8_t zeros[1000];
    b.len = 0;
    CHECK(lz4_block_compress(zeros, sizeof zeros, 1, &b) == 0);
    CHECK(b.len >= 6 && memcmp(b.data + b.len - 5, zeros, 5) == 0);
    b.len = 0;
    CHECK(lz4_block_compress(zeros, 12, 9, &b) == 0);
    CHECK(b.len == 13 && b.data[0] == 0xC0);
    sq_buf_free(&b);
    TEST_DONE("lz4");
}
