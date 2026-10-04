/* rANS (single and interleaved) and FSE/tANS: tables, the table
 * description format, and codecs that must land near the entropy. */
#include <math.h>

#include "fse.h"
#include "rans.h"
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
    /* Normalisation: sums to 2^log, keeps every symbol, uses -1 for the
     * rarest only when allowed. */
    uint32_t count[8] = {1000, 1, 0, 300, 2, 0, 0, 5000};
    fse_norm norm[8];
    CHECK(fse_normalize(count, 8, 6303, 6, 1, norm) == 0);
    int sum = 0, lows = 0;
    for (int s = 0; s < 8; s++) {
        sum += norm[s] < 0 ? 1 : norm[s];
        lows += norm[s] == -1;
        CHECK((count[s] == 0) == (norm[s] == 0));
    }
    CHECK(sum == 64 && lows == 2);
    CHECK(fse_normalize(count, 8, 6303, 6, 0, norm) == 0 && norm[1] == 1 && norm[4] == 1);

    /* The table description round-trips, zero runs included. */
    fse_norm zn[40] = {0};
    zn[0] = 10, zn[1] = -1, zn[5] = 7, zn[39] = 14;  /* sums to 32 */
    sq_buf b;
    sq_buf_init(&b);
    CHECK(fse_write_ncount(&b, zn, 40, 5) == 0);
    fse_norm back[256];
    unsigned nsyms, log;
    CHECK(fse_read_ncount(b.data, b.len, back, &nsyms, &log, 9, 256) == b.len);
    CHECK(nsyms == 40 && log == 5 && memcmp(back, zn, sizeof zn) == 0);

    /* RFC 8878's predefined literal-length distribution decodes to a
     * table whose first states match the RFC's listing (Appendix A). */
    static const fse_norm ll[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
                                    2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
    static fse_dtable dt;
    CHECK(fse_build_dtable(&dt, ll, 36, 6) == 0);
    CHECK(dt.e[0].symbol == 0 && dt.e[0].nbits == 4 && dt.e[0].base == 0);
    CHECK(dt.e[1].symbol == 0 && dt.e[1].nbits == 4 && dt.e[1].base == 16);
    CHECK(dt.e[2].symbol == 1 && dt.e[2].nbits == 5 && dt.e[2].base == 32);
    CHECK(dt.e[63].symbol == 32 && dt.e[63].nbits == 6 && dt.e[63].base == 0);
    fse_norm bad[2] = {20, 20};  /* sums to 40, not 32 */
    CHECK(fse_build_dtable(&dt, bad, 2, 5) != 0);

    /* Codecs: skewed, text-like, single-symbol, random, tiny, empty. */
    static uint8_t skew[100000], text[60000], one[3000], rnd[20000];
    fill_random(skew, sizeof skew, 51);
    for (size_t i = 0; i < sizeof skew; i++) {
        uint8_t r = skew[i], s = 0;
        while (r & 1 && s < 10)
            r >>= 1, s++;
        skew[i] = s;
    }
    for (size_t i = 0; i < sizeof text; i++)
        text[i] = (uint8_t)"the quick brown fox\n"[(i * 13 + i / 7) % 20];
    memset(one, 'q', sizeof one);
    fill_random(rnd, sizeof rnd, 52);
    struct { const uint8_t *p; size_t n; } in[] = {
        {skew, sizeof skew}, {text, sizeof text}, {one, sizeof one}, {rnd, sizeof rnd},
        {text, 1}, {text, 0}};
    for (size_t k = 0; k < sizeof in / sizeof in[0]; k++) {
        double h = entropy_bytes(in[k].p, in[k].n);
        size_t r1 = roundtrip(rans_compress, rans_decompress, in[k].p, in[k].n);
        size_t r2 = roundtrip(rans2_compress, rans2_decompress, in[k].p, in[k].n);
        size_t f = roundtrip(fse_compress, fse_decompress, in[k].p, in[k].n);
        CHECK(r1 && r2 && f);
        /* Quantised probabilities cost a little: within 1% + headers. */
        CHECK(r1 <= 8 + 512 + h * 1.01 + 16);
        CHECK(r2 <= 8 + 512 + h * 1.01 + 16);
        CHECK(f <= 8 + 300 + h * 1.01 + 16);
    }

    /* Corruption: bit flips decode to something or fail, never crash. */
    b.len = 0;
    fse_compress(text, 5000, &b);
    sq_buf d;
    sq_buf_init(&d);
    for (size_t i = 8; i < b.len; i += 5) {
        b.data[i] ^= 0x04;
        d.len = 0;
        fse_decompress(b.data, b.len, &d);
        b.data[i] ^= 0x04;
    }
    b.len = 0;
    rans2_compress(text, 5000, &b);
    for (size_t i = 8; i < b.len; i += 5) {
        b.data[i] ^= 0x04;
        d.len = 0;
        rans2_decompress(b.data, b.len, &d);
        b.data[i] ^= 0x04;
    }
    sq_buf_free(&b);
    sq_buf_free(&d);
    TEST_DONE("ans");
}
