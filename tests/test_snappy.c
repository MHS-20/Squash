#include "snappy.h"
#include "test.h"

typedef int (*decoder)(const uint8_t *, size_t, sq_buf *);

static int decodes_to(decoder f, const void *p, size_t n, const char *want)
{
    sq_buf b;
    sq_buf_init(&b);
    int err = f(p, n, &b);
    int ok = want ? !err && same_bytes(b.data, b.len, want, strlen(want)) : err != 0;
    sq_buf_free(&b);
    return ok;
}

int main(void)
{
    /* python-snappy: snappy.compress(b'hello hello hello hello, world').
     * Literal "hello ", copy(offset 6, length 17), literal ", world". */
    static const uint8_t ref[] = {30, 20, 'h', 'e', 'l', 'l', 'o', ' ', 66, 6, 0,
                                  24, ',', ' ', 'w', 'o', 'r', 'l', 'd'};
    const char *text = "hello hello hello hello, world";
    CHECK(decodes_to(sq_snappy_raw_decompress, ref, sizeof ref, text));
    sq_buf b;
    sq_buf_init(&b);
    CHECK(sq_snappy_raw_compress((const uint8_t *)text, strlen(text), 0, &b) == 0);
    CHECK(b.len == sizeof ref && memcmp(b.data, ref, sizeof ref) == 0);

    /* Varint preamble examples from format_description.txt. */
    static uint8_t big[2097150];
    b.len = 0;
    CHECK(sq_snappy_raw_compress(big, 64, 0, &b) == 0 && b.data[0] == 0x40);
    b.len = 0;
    CHECK(sq_snappy_raw_compress(big, sizeof big, 0, &b) == 0);
    CHECK(b.data[0] == 0xFE && b.data[1] == 0xFF && b.data[2] == 0x7F);

    /* Raw errors: length mismatch, offset 0, offset past the start,
     * literal running off the end, over-long varint. */
    CHECK(decodes_to(sq_snappy_raw_decompress, "\x05\x08" "abc", 5, NULL));
    CHECK(decodes_to(sq_snappy_raw_decompress, "\x05\x00" "a" "\x01\x00", 5, NULL));
    CHECK(decodes_to(sq_snappy_raw_decompress, "\x05\x00" "a" "\x01\x02", 5, NULL));
    CHECK(decodes_to(sq_snappy_raw_decompress, "\x05\x00" "a" "\x01\x01", 5, "aaaaa"));
    CHECK(decodes_to(sq_snappy_raw_decompress, "\x03\x08" "ab", 4, NULL));
    CHECK(decodes_to(sq_snappy_raw_decompress, "\xff\xff\xff\xff\xff\x01", 6, NULL));
    CHECK(decodes_to(sq_snappy_raw_decompress, "\x00", 1, ""));

    /* Framing: python-snappy's StreamCompressor().add_chunk(b'hello' * 3),
     * an uncompressed chunk with masked CRC-32C 0x628b0b80. */
    static const uint8_t frame[] = {0xff, 0x06, 0x00, 0x00, 's', 'N', 'a', 'P', 'p', 'Y',
                                    0x01, 0x13, 0x00, 0x00, 0x80, 0x0b, 0x8b, 0x62,
                                    'h', 'e', 'l', 'l', 'o', 'h', 'e', 'l', 'l', 'o',
                                    'h', 'e', 'l', 'l', 'o'};
    CHECK(decodes_to(sq_snappy_decompress, frame, sizeof frame, "hellohellohello"));
    uint8_t f[sizeof frame + 8];
    memcpy(f, frame, sizeof frame);
    f[14] ^= 1;  /* CRC */
    CHECK(decodes_to(sq_snappy_decompress, f, sizeof frame, NULL));
    memcpy(f, frame, sizeof frame);
    memcpy(f + sizeof frame, "\x80\x04\x00\x00skip", 8);  /* skippable chunk */
    CHECK(decodes_to(sq_snappy_decompress, f, sizeof f, "hellohellohello"));
    f[sizeof frame] = 0x02;  /* reserved unskippable chunk */
    CHECK(decodes_to(sq_snappy_decompress, f, sizeof f, NULL));
    CHECK(decodes_to(sq_snappy_decompress, frame + 10, sizeof frame - 10, NULL));  /* no id */
    sq_buf_free(&b);
    TEST_DONE("snappy");
}
