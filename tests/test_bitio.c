#include "bitio.h"
#include "test.h"

int main(void)
{
    /* LSB-first: values written low bit first round-trip, and the byte
     * layout matches DEFLATE's (first field in the low bits of byte 0). */
    sq_buf b;
    sq_buf_init(&b);
    bitwriter_lsb wl;
    bwl_init(&wl, &b);
    bwl_put(&wl, 1, 1);
    bwl_put(&wl, 2, 2);
    bwl_put(&wl, 0x1ABCD, 17);
    bwl_put(&wl, 0xFFFFFFFF, 32);
    CHECK(bwl_flush(&wl) == 0);
    CHECK_EQ_U(b.data[0] & 7, 0x5);  /* 1 then 2 (=binary 10) packed upward */
    bitreader_lsb rl;
    brl_init(&rl, b.data, b.len);
    CHECK_EQ_U(brl_read(&rl, 1), 1);
    CHECK_EQ_U(brl_read(&rl, 2), 2);
    CHECK_EQ_U(brl_read(&rl, 17), 0x1ABCD);
    CHECK_EQ_U(brl_read(&rl, 32), 0xFFFFFFFF);
    CHECK(!rl.overrun);
    brl_align(&rl);
    CHECK_EQ_U(brl_tell(&rl, b.data), b.len);
    brl_read(&rl, 1);
    CHECK(rl.overrun);  /* reading past the end is reported, not UB */

    /* MSB-first: bzip2 order, first bit lands in bit 7. */
    b.len = 0;
    bitwriter_msb wm;
    bwm_init(&wm, &b);
    bwm_put(&wm, 1, 1);
    bwm_put(&wm, 0x314159, 24);
    bwm_put(&wm, 0x265359, 24);
    CHECK(bwm_flush(&wm) == 0);
    CHECK_EQ_U(b.data[0], 0x98);  /* 1 followed by the top 7 bits of 0x31 */
    bitreader_msb rm;
    brm_init(&rm, b.data, b.len);
    CHECK_EQ_U(brm_read(&rm, 1), 1);
    CHECK_EQ_U(brm_read(&rm, 24), 0x314159);
    CHECK_EQ_U(brm_peek(&rm, 24), 0x265359);
    brm_consume(&rm, 24);
    brm_align(&rm);
    CHECK(!rm.overrun);
    CHECK_EQ_U(brm_tell(&rm, b.data), b.len);
    brm_read(&rm, 8);
    CHECK(rm.overrun);

    /* Long random sequence of mixed widths in both orders. */
    enum { N = 5000 };
    static uint32_t vals[N];
    static unsigned widths[N];
    unsigned char rnd[N * 5];
    fill_random(rnd, sizeof rnd, 1);
    for (int i = 0; i < N; i++) {
        widths[i] = 1 + rnd[i] % 32;
        vals[i] = (rnd[N + 4 * i] | (uint32_t)rnd[N + 4 * i + 1] << 8 |
                   (uint32_t)rnd[N + 4 * i + 2] << 16 | (uint32_t)rnd[N + 4 * i + 3] << 24) &
                  (uint32_t)((1ull << widths[i]) - 1);
    }
    b.len = 0;
    bwl_init(&wl, &b);
    for (int i = 0; i < N; i++)
        bwl_put(&wl, vals[i], widths[i]);
    bwl_flush(&wl);
    brl_init(&rl, b.data, b.len);
    int ok = 1;
    for (int i = 0; i < N; i++)
        ok &= brl_read(&rl, widths[i]) == vals[i];
    CHECK(ok && !rl.overrun);
    b.len = 0;
    bwm_init(&wm, &b);
    for (int i = 0; i < N; i++)
        bwm_put(&wm, vals[i], widths[i]);
    bwm_flush(&wm);
    brm_init(&rm, b.data, b.len);
    ok = 1;
    for (int i = 0; i < N; i++)
        ok &= brm_read(&rm, widths[i]) == vals[i];
    CHECK(ok && !rm.overrun);

    sq_buf_free(&b);
    TEST_DONE("bitio");
}
