/* CRC-64/XZ: the ECMA-182 polynomial, bit-reflected (0xC96C5795D7870F42),
 * table driven exactly like the 32-bit reflected CRC. */
#include <threads.h>

#include "checksum.h"

static uint64_t table[256];
static once_flag table_once = ONCE_FLAG_INIT;

static void build_table(void)
{
    for (uint64_t i = 0; i < 256; i++) {
        uint64_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xC96C5795D7870F42ull : c >> 1;
        table[i] = c;
    }
}

uint64_t sq_crc64(uint64_t crc, const void *data, size_t n)
{
    call_once(&table_once, build_table);
    const uint8_t *p = data;
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
