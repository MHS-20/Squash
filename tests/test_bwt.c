/* BWT (naive and SA-IS), suffix arrays, MTF and both RLEs. */
#include <stdint.h>

#include "bwt.h"
#include "mtf.h"
#include "rle.h"
#include "sais.h"
#include "test.h"

static int sa_is_sorted(const uint8_t *s, const int32_t *sa, int32_t n)
{
    static uint8_t seen[1 << 16];
    memset(seen, 0, (size_t)n);
    for (int32_t i = 0; i < n; i++) {
        if (sa[i] < 0 || sa[i] >= n || seen[sa[i]])
            return 0;
        seen[sa[i]] = 1;
    }
    for (int32_t i = 1; i < n; i++) {
        int32_t a = sa[i - 1], b = sa[i], la = n - a, lb = n - b;
        int c = memcmp(s + a, s + b, (size_t)(la < lb ? la : lb));
        if (c > 0 || (c == 0 && la > lb))
            return 0;
    }
    return 1;
}

static void inputs(int which, uint8_t *buf, int32_t *n)
{
    switch (which) {
    case 0: *n = 11; memcpy(buf, "abracadabra", 11); break;
    case 1: *n = 6; memcpy(buf, "banana", 6); break;
    case 2: *n = 1; buf[0] = 'z'; break;
    case 3: *n = 1000; memset(buf, 'a', 1000); break;                 /* one run */
    case 4: *n = 999; for (int i = 0; i < 999; i++) buf[i] = "abc"[i % 3]; break;
    case 5: *n = 3000; fill_random(buf, 3000, 8); for (int i = 0; i < 3000; i++) buf[i] &= 3; break;
    case 6: *n = 4096; fill_random(buf, 4096, 9); break;
    default: *n = 5000; for (int i = 0; i < 5000; i++) buf[i] = (uint8_t)("mississippi"[i % 11] + (i / 997)); break;
    }
}

int main(void)
{
    /* Textbook values. */
    uint8_t out[8192], back[8192];
    int32_t primary;
    CHECK(bwt_forward((const uint8_t *)"banana", 6, out, &primary) == 0);
    CHECK(memcmp(out, "nnbaaa", 6) == 0 && primary == 3);
    int32_t sa[16];
    CHECK(sais((const uint8_t *)"banana", sa, 6) == 0);
    int32_t want[6] = {5, 3, 1, 0, 4, 2};
    CHECK(memcmp(sa, want, sizeof want) == 0);

    static uint8_t buf[8192];
    static int32_t sabuf[8192];
    for (int w = 0; w < 8; w++) {
        int32_t n;
        inputs(w, buf, &n);
        CHECK(sais(buf, sabuf, n) == 0 && sa_is_sorted(buf, sabuf, n));
        uint8_t naive[8192];
        int32_t p1, p2;
        CHECK(bwt_naive(buf, n, naive, &p1) == 0);
        CHECK(bwt_forward(buf, n, out, &p2) == 0);
        CHECK(memcmp(naive, out, (size_t)n) == 0);  /* same last column */
        CHECK(bwt_inverse(out, n, p2, back) == 0 && memcmp(back, buf, (size_t)n) == 0);
        CHECK(bwt_inverse(naive, n, p1, back) == 0 && memcmp(back, buf, (size_t)n) == 0);
    }
    CHECK(bwt_inverse(out, 10, 10, back) == SQ_ERR_DATA);

    /* MTF: "bananaaa" after BWT-like clustering yields small indices. */
    uint8_t m[8], d[8];
    mtf_encode((const uint8_t *)"\x01\x01\x01\x02\x02\x01\x00\x00", 8, m);
    uint8_t mwant[8] = {1, 0, 0, 2, 0, 1, 2, 0};
    CHECK(memcmp(m, mwant, 8) == 0);
    mtf_decode(m, 8, d);
    CHECK(memcmp(d, "\x01\x01\x01\x02\x02\x01\x00\x00", 8) == 0);

    /* RLE1: runs of 4..255 become 4 bytes + count; longer runs split. */
    sq_buf b, r;
    sq_buf_init(&b);
    sq_buf_init(&r);
    static uint8_t run[600];
    memset(run, 'q', sizeof run);
    size_t used;
    CHECK(rle1_encode(run, sizeof run, &used, &b, 1000) == 0 && used == sizeof run);
    CHECK(b.len == 15);  /* 255 + 255 + 90: three 5-byte groups */
    CHECK(b.data[4] == 251 && b.data[14] == 86);
    CHECK(rle1_decode(b.data, b.len, &r) == 0 && same_bytes(r.data, r.len, run, sizeof run));
    b.len = 0;
    CHECK(rle1_encode(run, sizeof run, &used, &b, 12) == 0 && used == 510 && b.len == 10);
    b.len = r.len = 0;
    CHECK(rle1_encode((const uint8_t *)"aaab", 4, &used, &b, 100) == 0 && b.len == 4);

    /* RLE2: zero runs 1..6 are A, B, AA, BA, AB, BB. */
    uint16_t sym[64];
    uint8_t zeros[6] = {0};
    const char *codes[] = {"A", "B", "AA", "BA", "AB", "BB"};
    for (int len = 1; len <= 6; len++) {
        size_t k = rle2_encode(zeros, (size_t)len, sym);
        int ok = k == strlen(codes[len - 1]);
        for (size_t i = 0; ok && i < k; i++)
            ok = sym[i] == (codes[len - 1][i] == 'A' ? RLE2_RUNA : RLE2_RUNB);
        CHECK(ok);
    }
    static uint8_t mt[5000], mback[5000];
    static uint16_t syms[5000];
    for (int i = 0; i < 5000; i++)
        mt[i] = (i % 97 < 60) ? 0 : (uint8_t)(i % 7);
    size_t k = rle2_encode(mt, 5000, syms);
    CHECK(rle2_decode(syms, k, mback, 5000) == 5000 && memcmp(mt, mback, 5000) == 0);
    CHECK(rle2_decode(syms, k, mback, 4999) == SQ_ERR_DATA);
    for (int i = 0; i < 64; i++)
        sym[i] = RLE2_RUNB;  /* 2^65 - 2 zeros: must be refused, not wrap */
    CHECK(rle2_decode(sym, 64, mback, 5000) == SQ_ERR_DATA);

    sq_buf_free(&b);
    sq_buf_free(&r);
    TEST_DONE("bwt");
}
