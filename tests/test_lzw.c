#include "squash.h"
#include "test.h"

static int decodes_to(const uint8_t *p, size_t n, const char *want)
{
    sq_buf b;
    sq_buf_init(&b);
    int err = sq_lzw_decompress(p, n, &b);
    int ok = want ? !err && same_bytes(b.data, b.len, want, strlen(want)) : err != 0;
    sq_buf_free(&b);
    return ok;
}

int main(void)
{
    /* `printf 'hello hello hello\n' | compress` (ncompress 5.0). Without a
     * table reset our encoder must produce the very same bytes. */
    static const uint8_t ref[] = {0x1f, 0x9d, 0x90, 0x68, 0xca, 0xb0, 0x61, 0xf3, 0x06,
                                  0x44, 0xc0, 0x81, 0x05, 0x0f, 0x12, 0x54, 0x00};
    const char *text = "hello hello hello\n";
    CHECK(decodes_to(ref, sizeof ref, text));
    sq_buf b;
    sq_buf_init(&b);
    CHECK(sq_lzw_compress((const uint8_t *)text, strlen(text), 16, &b) == 0);
    CHECK(b.len == sizeof ref && memcmp(b.data, ref, sizeof ref) == 0);

    /* Old non-block-mode stream (flags without 0x80): code 256 is an
     * ordinary entry, here the KwKwK case 'a' + "aa". */
    static const uint8_t old[] = {0x1f, 0x9d, 0x10, 0x61, 0x00, 0x02};
    CHECK(decodes_to(old, sizeof old, "aaa"));

    /* Header and code errors. */
    static const uint8_t badbits[] = {0x1f, 0x9d, 0x91};       /* 17-bit codes */
    static const uint8_t first_big[] = {0x1f, 0x9d, 0x90, 0x00, 0x03}; /* first code 384 */
    static const uint8_t ahead[] = {0x1f, 0x9d, 0x90, 0x61, 0x04, 0x02}; /* 97 then 258 */
    CHECK(decodes_to(badbits, sizeof badbits, NULL));
    CHECK(decodes_to(first_big, sizeof first_big, NULL));
    CHECK(decodes_to(ahead, sizeof ahead, NULL));
    CHECK(decodes_to((const uint8_t *)"\x1f\x9d\x90", 3, ""));  /* empty */

    /* Width changes and table resets across every maximum width. */
    static uint8_t data[300000];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)("abcdefgh"[(i * i / 7 + i / 13) % 8]);
    fill_random(data + 200000, 50000, 5);
    for (int bits = 9; bits <= 16; bits++) {
        b.len = 0;
        sq_buf d;
        sq_buf_init(&d);
        CHECK(sq_lzw_compress(data, sizeof data, bits, &b) == 0);
        CHECK(sq_lzw_decompress(b.data, b.len, &d) == 0);
        CHECK(d.len == sizeof data && memcmp(d.data, data, d.len) == 0);
        sq_buf_free(&d);
    }
    CHECK(sq_lzw_compress(data, 10, 8, &b) == SQ_ERR_ARG);
    sq_buf_free(&b);
    TEST_DONE("lzw");
}
