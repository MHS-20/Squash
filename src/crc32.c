/* Table-driven CRCs: one 256-entry table per polynomial, each entry being
 * the CRC of a single byte, so the hot loop consumes a byte per lookup. */
#include <threads.h>

#include "checksum.h"

static uint32_t crc32_table[256];   /* reflected 0xEDB88320 */
static uint32_t crc32c_table[256];  /* reflected 0x82F63B78 */
static uint32_t bzip2_table[256];   /* MSB-first 0x04C11DB7 */
static once_flag tables_once = ONCE_FLAG_INIT;

static void build_tables(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        /* Reflected CRCs process the low bit first, so they shift right. */
        uint32_t a = i, c = i;
        for (int k = 0; k < 8; k++) {
            a = (a & 1) ? (a >> 1) ^ 0xEDB88320u : a >> 1;
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        }
        crc32_table[i] = a;
        crc32c_table[i] = c;
        /* bzip2 feeds the high bit first, so its register shifts left. */
        uint32_t b = i << 24;
        for (int k = 0; k < 8; k++)
            b = (b & 0x80000000u) ? (b << 1) ^ 0x04C11DB7u : b << 1;
        bzip2_table[i] = b;
    }
}

/* Pre- and post-inversion live inside the function so callers can chain
 * calls with the finished value, like zlib's crc32(). */
static uint32_t reflected(const uint32_t *t, uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t sq_crc32(uint32_t crc, const void *data, size_t n)
{
    call_once(&tables_once, build_tables);
    return reflected(crc32_table, crc, data, n);
}

uint32_t sq_crc32c(uint32_t crc, const void *data, size_t n)
{
    call_once(&tables_once, build_tables);
    return reflected(crc32c_table, crc, data, n);
}

uint32_t sq_crc32_bzip2(uint32_t crc, const void *data, size_t n)
{
    call_once(&tables_once, build_tables);
    const uint8_t *p = data;
    crc = ~crc;
    while (n--)
        crc = (crc << 8) ^ bzip2_table[(crc >> 24) ^ *p++];
    return ~crc;
}
