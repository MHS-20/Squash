/* zstd: reference vectors, the RFC's worked examples (Huffman codes,
 * repeat offsets), error paths, and encoder output checked by the real
 * zstd when it is installed. */
#include "zstd.h"
#include "test.h"

static int decodes_to(const uint8_t *p, size_t n, const char *want)
{
    sq_buf b;
    sq_buf_init(&b);
    int err = sq_zstd_decompress(p, n, &b);
    int ok = want ? !err && same_bytes(b.data, b.len, want, strlen(want)) : err != 0;
    sq_buf_free(&b);
    return ok;
}

int main(void)
{
    /* `printf hello | zstd`: window descriptor, one raw block, checksum.
     * Then the same with --no-check --no-content-size. */
    static const uint8_t ref[] = {0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x58, 0x29, 0x00, 0x00,
                                  0x68, 0x65, 0x6c, 0x6c, 0x6f, 0xa3, 0x6d, 0x9f, 0x88};
    static const uint8_t bare[] = {0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x58, 0x29, 0x00, 0x00,
                                   0x68, 0x65, 0x6c, 0x6c, 0x6f};
    CHECK(decodes_to(ref, sizeof ref, "hello"));
    CHECK(decodes_to(bare, sizeof bare, "hello"));
    uint8_t bad[sizeof ref];
    memcpy(bad, ref, sizeof bad);
    bad[17] ^= 1;  /* checksum */
    CHECK(decodes_to(bad, sizeof bad, NULL));
    memcpy(bad, ref, sizeof bad);
    bad[4] |= 0x08;  /* reserved bit */
    CHECK(decodes_to(bad, sizeof bad, NULL));
    memcpy(bad, ref, sizeof bad);
    bad[6] |= 0x06;  /* reserved block type */
    CHECK(decodes_to(bad, sizeof bad, NULL));
    CHECK(decodes_to(ref, sizeof ref - 1, NULL));
    /* Skippable frame, then two frames. */
    uint8_t cat[8 + 3 + 2 * sizeof ref] = {0x5a, 0x2a, 0x4d, 0x18, 3, 0, 0, 0, 'x', 'y', 'z'};
    memcpy(cat + 11, ref, sizeof ref);
    memcpy(cat + 11 + sizeof ref, ref, sizeof ref);
    CHECK(decodes_to(cat, sizeof cat, "hellohello"));
    /* An empty RLE block as the whole frame (found by fuzzing). */
    static const uint8_t empty_rle[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x00, 0x03, 0x00, 0x00, 'x'};
    CHECK(decodes_to(empty_rle, sizeof empty_rle, ""));

    /* The format document's Huffman example: weights A=4 B=3 C=2 D=0 E=1
     * (F implied 1) in direct form, giving A=1 B=01 C=001 E=0000 F=0001.
     * "ABEF" written backwards with those codes is 0x01 0x0D. (The
     * document's example stream, 0x10 0x0D, swaps the codes of E and F
     * relative to its own code table; real zstd follows the table.) */
    static const uint8_t table[] = {127 + 5, 0x43, 0x20, 0x10};
    static huf0_dtable ht;
    CHECK(huf0_read_table(&ht, table, sizeof table) == sizeof table);
    CHECK(ht.max_bits == 4);
    uint8_t out[4];
    CHECK(huf0_decode_stream(&ht, (const uint8_t *)"\x01\x0d", 2, out, 4) == 0);
    CHECK(out[0] == 0 && out[1] == 1 && out[2] == 4 && out[3] == 5);
    CHECK(huf0_decode_stream(&ht, (const uint8_t *)"\x01\x0d", 2, out, 3) != 0);  /* leftover */

    /* RFC 8878 repeat-offset walkthrough (section 3.1.2.5). */
    uint32_t rep[3] = {1, 4, 8};
    struct { uint32_t ov, ll, r0, r1, r2; } steps[] = {
        {1114, 11, 1111, 1, 4}, {1, 22, 1111, 1, 4}, {2225, 22, 2222, 1111, 1},
        {1114, 111, 1111, 2222, 1111}, {3336, 33, 3333, 1111, 2222}, {2, 22, 1111, 3333, 2222},
        {3, 33, 2222, 1111, 3333}, {3, 0, 2221, 2222, 1111}, {1, 0, 2222, 2221, 1111}};
    int ok = 1;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        zstd_resolve_offset(rep, steps[i].ov, steps[i].ll);
        ok &= rep[0] == steps[i].r0 && rep[1] == steps[i].r1 && rep[2] == steps[i].r2;
    }
    CHECK(ok);
    uint32_t one[3] = {1, 4, 8};
    CHECK(zstd_resolve_offset(one, 3, 0) == 0);  /* rep1 - 1 == 0 is corrupt */

    /* Encoder: Huffman literals (1 and 4 streams), FSE and predefined
     * tables, repeat offsets, several blocks, RLE and raw blocks. */
    static uint8_t data[400000];
    fill_random(data, 50000, 71);
    for (size_t i = 50000; i < sizeof data; i++)
        data[i] = (i % 7000 < 200) ? 'z' : (uint8_t)("compressible text "[(i * 5 + i / 11) % 18]);
    sq_buf z, d;
    sq_buf_init(&z);
    sq_buf_init(&d);
    int levels[] = {1, 3, 9, 19};
    for (size_t k = 0; k < 4; k++) {
        z.len = d.len = 0;
        CHECK(sq_zstd_compress(data, sizeof data, levels[k], &z) == 0);
        CHECK(sq_zstd_decompress(z.data, z.len, &d) == 0 && same_bytes(d.data, d.len, data, sizeof data));
        int r = tool_decodes("zstd", "zstd -dc", z.data, z.len, data, sizeof data);
        if (r < 0 && k == 0)
            printf("zstd           SKIPPED official check (zstd not found)\n");
        if (r >= 0)
            CHECK(r == 1);
    }
    for (size_t n = 0; n < 40; n++) {  /* tiny inputs: every header size path */
        z.len = d.len = 0;
        CHECK(sq_zstd_compress(data + 60000, n * 7, 3, &z) == 0);
        CHECK(sq_zstd_decompress(z.data, z.len, &d) == 0 && same_bytes(d.data, d.len, data + 60000, n * 7));
    }
    sq_buf_free(&z);
    sq_buf_free(&d);
    TEST_DONE("zstd");
}
