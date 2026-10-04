#include "mtf.h"

#include <string.h>

static void init(uint8_t list[256])
{
    for (int i = 0; i < 256; i++)
        list[i] = (uint8_t)i;
}

void mtf_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t list[256];
    init(list);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        int k = 0;
        while (list[k] != c)
            k++;
        memmove(list + 1, list, (size_t)k);
        list[0] = c;
        out[i] = (uint8_t)k;
    }
}

void mtf_decode(const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t list[256];
    init(list);
    for (size_t i = 0; i < n; i++) {
        int k = in[i];
        uint8_t c = list[k];
        memmove(list + 1, list, (size_t)k);
        list[0] = c;
        out[i] = c;
    }
}
