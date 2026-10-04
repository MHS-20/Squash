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
    TEST_DONE("checksum");
}
