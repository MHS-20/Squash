#include "huffman.h"
#include "test.h"

/* Kraft sum scaled by 2^max: equals 2^max exactly for a complete code. */
static unsigned long long kraft(const uint8_t *len, int n, int max)
{
    unsigned long long s = 0;
    for (int i = 0; i < n; i++)
        if (len[i])
            s += 1ull << (max - len[i]);
    return s;
}

int main(void)
{
    /* RFC 1951 section 3.2.2 worked example: ABCDEFGH with these lengths. */
    uint8_t rfc_len[8] = {3, 3, 3, 3, 3, 2, 4, 4};
    uint32_t code[HUFF_MAX_SYMS];
    huff_codes(rfc_len, 8, code);
    uint32_t want[8] = {2, 3, 4, 5, 6, 0, 14, 15};
    for (int i = 0; i < 8; i++)
        CHECK_EQ_U(code[i], want[i]);
    CHECK_EQ_U(huff_reverse(0x6, 4), 0x6);
    CHECK_EQ_U(huff_reverse(0x1, 5), 0x10);

    /* Lengths are a complete prefix code and follow the frequencies. */
    uint32_t freq[6] = {45, 13, 12, 16, 9, 5};
    uint8_t len[HUFF_MAX_SYMS];
    huff_lengths(freq, 6, 15, len);
    CHECK_EQ_U(kraft(len, 6, 15), 1u << 15);
    CHECK_EQ_U(len[0], 1);  /* the dominant symbol gets the shortest code */

    /* Zero frequencies get no code; a lone symbol still gets one bit. */
    uint32_t one[4] = {0, 7, 0, 0};
    huff_lengths(one, 4, 15, len);
    CHECK(len[0] == 0 && len[1] == 1 && len[2] == 0);

    /* Fibonacci weights force a degenerate, deep tree; the limit must hold
     * and the code must stay complete. */
    uint32_t fib[30];
    fib[0] = fib[1] = 1;
    for (int i = 2; i < 30; i++)
        fib[i] = fib[i - 1] + fib[i - 2];
    huff_lengths(fib, 30, 30, len);
    int deepest = 0;
    for (int i = 0; i < 30; i++)
        deepest = len[i] > deepest ? len[i] : deepest;
    CHECK(deepest > 15);
    huff_lengths(fib, 30, 7, len);
    deepest = 0;
    for (int i = 0; i < 30; i++)
        deepest = len[i] > deepest ? len[i] : deepest;
    CHECK(deepest <= 7);
    CHECK_EQ_U(kraft(len, 30, 7), 1u << 7);

    /* Over-subscribed lengths (three 1-bit codes) are rejected. */
    static huff_table t;
    huff_canon c;
    uint8_t bad[3] = {1, 1, 1};
    CHECK(huff_table_build(&t, bad, 3, 15) != 0);
    CHECK(huff_canon_build(&c, bad, 3) != 0);

    /* Encode random symbols with a skewed code, decode with both decoders. */
    enum { NSYM = 286, N = 20000 };
    uint32_t f[NSYM];
    unsigned char rnd[N];
    fill_random(rnd, N, 7);
    for (int i = 0; i < NSYM; i++)
        f[i] = 1 + (i % 17) * (i % 5) * 3;
    huff_lengths(f, NSYM, 15, len);
    huff_codes(len, NSYM, code);
    static int syms[N];
    for (int i = 0; i < N; i++)
        syms[i] = (rnd[i] * 7 + i) % NSYM;

    sq_buf b;
    sq_buf_init(&b);
    bitwriter_lsb wl;
    bwl_init(&wl, &b);
    for (int i = 0; i < N; i++)
        bwl_put(&wl, huff_reverse(code[syms[i]], len[syms[i]]), len[syms[i]]);
    bwl_flush(&wl);
    CHECK(huff_table_build(&t, len, NSYM, 15) == 0);
    bitreader_lsb rl;
    brl_init(&rl, b.data, b.len);
    int ok = 1;
    for (int i = 0; i < N; i++)
        ok &= huff_decode_lsb(&t, &rl) == syms[i];
    CHECK(ok && !rl.overrun);

    b.len = 0;
    bitwriter_msb wm;
    bwm_init(&wm, &b);
    for (int i = 0; i < N; i++)
        bwm_put(&wm, code[syms[i]], len[syms[i]]);
    bwm_flush(&wm);
    CHECK(huff_canon_build(&c, len, NSYM) == 0);
    bitreader_msb rm;
    brm_init(&rm, b.data, b.len);
    ok = 1;
    for (int i = 0; i < N; i++)
        ok &= huff_decode_msb(&c, &rm) == syms[i];
    CHECK(ok && !rm.overrun);

    /* An incomplete code leaves holes that decode as errors, not garbage. */
    uint8_t inc[2] = {2, 2};  /* only 00 and 01 exist */
    CHECK(huff_table_build(&t, inc, 2, 15) == 0);
    unsigned char ones = 0xFF;
    brl_init(&rl, &ones, 1);
    CHECK(huff_decode_lsb(&t, &rl) == -1);

    sq_buf_free(&b);
    TEST_DONE("huffman");
}
