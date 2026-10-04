#include "checksum.h"

#define ADLER_MOD 65521u  /* largest prime below 2^16 */
/* Largest run that cannot overflow 32 bits before reducing:
 * 255*n*(n+1)/2 + (n+1)*(ADLER_MOD-1) <= 2^32-1. Deferring the modulo this
 * long is what makes Adler-32 cheap. */
#define ADLER_NMAX 5552

uint32_t sq_adler32(uint32_t adler, const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (n > 0) {
        size_t run = n < ADLER_NMAX ? n : ADLER_NMAX;
        n -= run;
        while (run--) {
            a += *p++;
            b += a;
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
    }
    return (b << 16) | a;
}
