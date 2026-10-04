#include "bzip2.h"
#include "test.h"

static int decodes_to(const uint8_t *p, size_t n, const char *want)
{
    sq_buf b;
    sq_buf_init(&b);
    int err = sq_bzip2_decompress(p, n, &b);
    int ok = want ? !err && same_bytes(b.data, b.len, want, strlen(want)) : err != 0;
    sq_buf_free(&b);
    return ok;
}

int main(void)
{
    /* `printf '' | bzip2` and `printf 'hello\n' | bzip2`. */
    static const uint8_t empty[] = {0x42, 0x5a, 0x68, 0x39, 0x17, 0x72, 0x45,
                                    0x38, 0x50, 0x90, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t hello[] = {
        0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0xc1, 0xc0, 0x80, 0xe2,
        0x00, 0x00, 0x01, 0x41, 0x00, 0x00, 0x10, 0x02, 0x44, 0xa0, 0x00, 0x30, 0xcd, 0x00,
        0xc3, 0x46, 0x29, 0x97, 0x17, 0x72, 0x45, 0x38, 0x50, 0x90, 0xc1, 0xc0, 0x80, 0xe2};
    CHECK(decodes_to(empty, sizeof empty, ""));
    CHECK(decodes_to(hello, sizeof hello, "hello\n"));
    sq_buf b;
    sq_buf_init(&b);
    CHECK(sq_bzip2_compress(NULL, 0, 9, &b) == 0);
    CHECK(b.len == sizeof empty && memcmp(b.data, empty, sizeof empty) == 0);

    uint8_t bad[sizeof hello];
    memcpy(bad, hello, sizeof bad);
    bad[10] ^= 0x80;  /* block CRC */
    CHECK(decodes_to(bad, sizeof bad, NULL));
    memcpy(bad, hello, sizeof bad);
    bad[sizeof bad - 1] ^= 1;  /* combined CRC */
    CHECK(decodes_to(bad, sizeof bad, NULL));
    memcpy(bad, hello, sizeof bad);
    bad[3] = '0';  /* block size digit */
    CHECK(decodes_to(bad, sizeof bad, NULL));
    memcpy(bad, hello, sizeof bad);
    bad[16] |= 0x0F;  /* origPtr far beyond the block */
    CHECK(decodes_to(bad, sizeof bad, NULL));
    uint8_t two[2 * sizeof hello];  /* concatenated streams */
    memcpy(two, hello, sizeof hello);
    memcpy(two + sizeof hello, hello, sizeof hello);
    CHECK(decodes_to(two, sizeof two, "hello\nhello\n"));

    /* Several blocks at level 1, with runs long enough to exercise RLE1
     * and the RUNA/RUNB coding, and the randomised-block path. */
    static uint8_t data[350000];
    fill_random(data, sizeof data, 21);
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (i / 1000) % 3 == 0 ? 'r' : (uint8_t)('a' + data[i] % 6);
    for (int rnd = 0; rnd <= 1; rnd++) {
        b.len = 0;
        sq_buf d;
        sq_buf_init(&d);
        CHECK(bzip2_compress_ex(data, sizeof data, 1, rnd, &b) == 0);
        CHECK(sq_bzip2_decompress(b.data, b.len, &d) == 0);
        CHECK(same_bytes(d.data, d.len, data, sizeof data));
        /* The real bzip2 must agree, in particular on derandomisation. */
        int official = tool_decodes("bzip2", "bzip2 -dc", b.data, b.len, data, sizeof data);
        if (official < 0)
            printf("bzip2          SKIPPED official check (bzip2 not found)\n");
        else
            CHECK(official == 1);
        sq_buf_free(&d);
    }
    sq_buf_free(&b);
    TEST_DONE("bzip2");
}
