#include "checksum.h"
#include "test.h"

int main(void)
{
    const char *s = "123456789";  /* the standard CRC "check" input */
    CHECK_EQ_U(sq_crc32(0, s, 9), 0xCBF43926);
    CHECK_EQ_U(sq_crc32c(0, s, 9), 0xE3069283);
    CHECK_EQ_U(sq_crc32_bzip2(0, s, 9), 0xFC891918);  /* CRC-32/BZIP2 */
    CHECK_EQ_U(sq_crc32(0, "", 0), 0);
    /* Chaining must equal one pass. */
    CHECK_EQ_U(sq_crc32(sq_crc32(0, s, 4), s + 4, 5), 0xCBF43926);
    CHECK_EQ_U(sq_crc32_bzip2(sq_crc32_bzip2(0, s, 4), s + 4, 5), 0xFC891918);

    CHECK_EQ_U(sq_crc64(0, s, 9), 0x995DC9BBDF1939FAull);  /* CRC-64/XZ */
    CHECK_EQ_U(sq_crc64(sq_crc64(0, s, 2), s + 2, 7), 0x995DC9BBDF1939FAull);

    /* FIPS 180-2 examples: one block, and the two-block 56-byte message. */
    uint8_t dg[32];
    sq_sha256("abc", 3, dg);
    CHECK_EQ_U(dg[0], 0xba);
    CHECK_EQ_U(dg[31], 0xad);
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sq_sha256(m2, strlen(m2), dg);
    CHECK_EQ_U(dg[0], 0x24);
    CHECK_EQ_U(dg[31], 0xc1);

    CHECK_EQ_U(sq_adler32(1, "Wikipedia", 9), 0x11E60398);
    CHECK_EQ_U(sq_adler32(1, "", 0), 1);

    CHECK_EQ_U(sq_xxh32("", 0, 0), 0x02CC5D05);
    CHECK_EQ_U(sq_xxh32("abc", 3, 0), 0x32D153FF);
    CHECK_EQ_U(sq_xxh64("", 0, 0), 0xEF46DB3751D8E999ull);
    CHECK_EQ_U(sq_xxh64("abc", 3, 0), 0x44BC2CF5AD770999ull);

    /* Long inputs exercise the lane loops and the Adler NMAX deferral;
     * expected values come from Python's zlib and the xxhsum tool. */
    static unsigned char big[100000];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (unsigned char)(i * 7 + (i >> 8));
    CHECK_EQ_U(sq_crc32(0, big, sizeof big), 0xE72BDDC1);
    CHECK_EQ_U(sq_adler32(1, big, sizeof big), 0xD43B9AEF);
    CHECK_EQ_U(sq_xxh32(big, sizeof big, 0), 0x7E664BBD);
    CHECK_EQ_U(sq_xxh64(big, sizeof big, 0), 0xD88605C21700AF94ull);
    CHECK_EQ_U(sq_xxh32(big, 37, 0), 0x80228200);
    CHECK_EQ_U(sq_xxh64(big, 37, 0), 0x0F67F332BDD5B35Bull);
    static const uint8_t big_sha[32] = {0x55,0xaf,0x39,0x4c,0x98,0x0c,0x7a,0x7f,0xb6,0x8a,0xa9,0x04,0xc4,0xaf,0xdd,0x93,0xd7,0x6e,0x5f,0x82,0x64,0x87,0x10,0x5f,0xc0,0x6f,0x92,0xa2,0x5b,0xab,0x8c,0xbe};
    sq_sha256(big, sizeof big, dg);
    CHECK(memcmp(dg, big_sha, 32) == 0);
    CHECK_EQ_U(sq_crc64(0, big, sizeof big), 0xe13ecbc474c35b8dull);
    TEST_DONE("checksum");
}
