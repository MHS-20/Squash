/* Static and adaptive arithmetic coding: round trips, and output size close
 * to the order-0 entropy, which is the whole point of arithmetic coding. */
#include <math.h>

#include "arith.h"
#include "test.h"

static double entropy_bytes(const uint8_t *p, size_t n)
{
    double count[256] = {0}, h = 0;
    for (size_t i = 0; i < n; i++)
        count[p[i]]++;
    for (int s = 0; s < 256; s++)
        if (count[s])
            h -= count[s] * log2(count[s] / (double)n);
    return h / 8;
}

typedef int (*codec)(const uint8_t *, size_t, sq_buf *);

static size_t roundtrip(codec enc, codec dec, const uint8_t *p, size_t n)
{
    sq_buf z, d;
    sq_buf_init(&z);
    sq_buf_init(&d);
    int ok = enc(p, n, &z) == 0 && dec(z.data, z.len, &d) == 0 && same_bytes(d.data, d.len, p, n);
    size_t size = ok ? z.len : 0;
    sq_buf_free(&z);
    sq_buf_free(&d);
    return size;
}

int main(void)
{
    static uint8_t skew[200000], text[100000], one[5000], rnd[30000];
    /* Geometric-ish distribution: entropy well under 8 bits. */
    fill_random(skew, sizeof skew, 31);
    for (size_t i = 0; i < sizeof skew; i++) {
        uint8_t r = skew[i], s = 0;
        while (r & 1 && s < 12)
            r >>= 1, s++;
        skew[i] = s;
    }
    for (size_t i = 0; i < sizeof text; i++)
        text[i] = (uint8_t)"etaoin shrdlu\n"[(i * 7 + i / 3) % 14];
    memset(one, 'x', sizeof one);
    fill_random(rnd, sizeof rnd, 32);

    struct { const uint8_t *p; size_t n; } in[] = {
        {skew, sizeof skew}, {text, sizeof text}, {one, sizeof one}, {rnd, sizeof rnd},
        {text, 0}, {text, 1}};
    for (size_t k = 0; k < sizeof in / sizeof in[0]; k++) {
        double h = entropy_bytes(in[k].p, in[k].n);
        size_t s = roundtrip(arith_static_compress, arith_static_decompress, in[k].p, in[k].n);
        size_t a = roundtrip(arith_adaptive_compress, arith_adaptive_decompress, in[k].p, in[k].n);
        CHECK(s > 0 && a > 0);
        /* Static: header + within 0.5% (+ a few bytes) of the entropy. */
        CHECK(s <= 8 + 512 + h * 1.005 + 8);
        /* Adaptive pays a learning cost, but stays close on large inputs. */
        if (in[k].n >= 100000)
            CHECK(a <= 8 + h * 1.03 + 64);
    }

    /* Corrupt streams decode to something or fail, never crash. */
    sq_buf z, d;
    sq_buf_init(&z);
    sq_buf_init(&d);
    CHECK(arith_adaptive_compress(text, 2000, &z) == 0);
    for (size_t i = 8; i < z.len; i += 3) {
        z.data[i] ^= 0x10;
        d.len = 0;
        arith_adaptive_decompress(z.data, z.len, &d);
        z.data[i] ^= 0x10;
    }
    CHECK(arith_static_decompress(z.data, 10, &d) == SQ_ERR_DATA);
    sq_buf_free(&z);
    sq_buf_free(&d);
    TEST_DONE("arith");
}
