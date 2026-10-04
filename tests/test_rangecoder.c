#include "rangecoder.h"
#include "test.h"

int main(void)
{
    enum { N = 50000 };
    static uint8_t bits[N];
    static uint32_t vals[N];
    fill_random(bits, N, 41);
    for (int i = 0; i < N; i++) {
        vals[i] = (uint32_t)bits[i] * 2654435761u;
        bits[i] = (bits[i] % 10) < 9;  /* 90% ones: compressible */
    }

    /* Mixed stream: modelled bits, direct bits, bit trees both ways. */
    sq_buf b;
    sq_buf_init(&b);
    rc_encoder e;
    rc_enc_init(&e, &b);
    rc_prob p1 = RC_PROB_INIT, tree[256], rtree[16];
    rc_probs_init(tree, 256);
    rc_probs_init(rtree, 16);
    for (int i = 0; i < N; i++) {
        rc_enc_bit(&e, &p1, bits[i]);
        if (i % 7 == 0) {
            rc_enc_direct(&e, vals[i] >> 6, 26);
            rc_enc_tree(&e, tree, 8, vals[i] & 0xFF);
            rc_enc_tree_rev(&e, rtree, 4, vals[i] >> 28);
        }
    }
    CHECK(rc_enc_flush(&e) == 0);
    CHECK(b.data[0] == 0);
    /* 50000 bits at 90/10 is ~0.47 bits each, plus ~38 bits per 7th step. */
    CHECK(b.len < (N * 0.47 + N / 7 * 38.5) / 8 + 64);

    rc_decoder d;
    CHECK(rc_dec_init(&d, b.data, b.len) == 0);
    p1 = RC_PROB_INIT;
    rc_probs_init(tree, 256);
    rc_probs_init(rtree, 16);
    int ok = 1;
    for (int i = 0; i < N && ok; i++) {
        ok &= rc_dec_bit(&d, &p1) == bits[i];
        if (i % 7 == 0) {
            ok &= rc_dec_direct(&d, 26) == vals[i] >> 6;
            ok &= rc_dec_tree(&d, tree, 8) == (vals[i] & 0xFF);
            ok &= rc_dec_tree_rev(&d, rtree, 4) == vals[i] >> 28;
        }
    }
    CHECK(ok);
    CHECK(!d.overrun && rc_dec_pos(&d, b.data) == b.len);

    /* A carry must propagate through queued 0xFF bytes: drive `low` up
     * with long runs of improbable bits. */
    b.len = 0;
    rc_enc_init(&e, &b);
    rc_prob p = RC_PROB_INIT;
    for (int i = 0; i < 20000; i++)
        rc_enc_bit(&e, &p, (i % 97) != 0);
    rc_enc_flush(&e);
    rc_dec_init(&d, b.data, b.len);
    p = RC_PROB_INIT;
    ok = 1;
    for (int i = 0; i < 20000; i++)
        ok &= rc_dec_bit(&d, &p) == ((i % 97) != 0);
    CHECK(ok);

    CHECK(rc_dec_init(&d, (const uint8_t *)"\x01\0\0\0\0", 5) == SQ_ERR_DATA);
    CHECK(rc_dec_init(&d, (const uint8_t *)"\0\0", 2) == SQ_ERR_DATA);
    CHECK(rc_bit_price(RC_PROB_INIT, 0) == 16);  /* p = 1/2 costs one bit */
    sq_buf_free(&b);
    TEST_DONE("rangecoder");
}
