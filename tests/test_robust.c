/* Corrupted input must fail cleanly: truncations and bit flips of valid
 * streams may decode to anything or to an error, but never crash, hang or
 * touch memory they should not (run under `make SAN=1 test` to enforce the
 * last part). A truncated stream must never decode to the original. */
#include "codec.h"
#include "corpus.h"

static int decode(const sq_codec *c, const unsigned char *p, size_t n, sq_buf *out)
{
    out->len = 0;
    return c->decompress(p, n, out);
}

int main(void)
{
    unsigned char text[20000], rnd[3000];
    make_text(text, sizeof text, 1);
    fill_random(rnd, sizeof rnd, 2);
    struct { const unsigned char *p; size_t n; } inputs[] = {
        {text, sizeof text}, {rnd, sizeof rnd}, {text, 100}};

    unsigned char flips[4096];
    fill_random(flips, sizeof flips, 99);
    for (size_t c = 0; c < sq_codec_count; c++) {
        const sq_codec *codec = &sq_codecs[c];
        int truncations_ok = 1, flips_done = 0;
        for (size_t k = 0; k < sizeof inputs / sizeof inputs[0]; k++) {
            sq_buf z, d;
            sq_buf_init(&z);
            sq_buf_init(&d);
            CHECK(codec->compress(inputs[k].p, inputs[k].n, 0, &z) == 0);
            unsigned char *copy = malloc(z.len);
            /* Every strict prefix (stepping through large streams). */
            size_t step = z.len > 2000 ? 7 : 1;
            for (size_t cut = 0; cut < z.len; cut += step) {
                memcpy(copy, z.data, cut);
                int err = decode(codec, copy, cut, &d);
                if (!err && same_bytes(d.data, d.len, inputs[k].p, inputs[k].n))
                    truncations_ok = 0;
            }
            /* Single and multiple bit flips at pseudo-random positions. */
            for (int t = 0; t < 600; t++) {
                memcpy(copy, z.data, z.len);
                int nflip = 1 + t % 3;
                for (int f = 0; f < nflip; f++) {
                    size_t at = ((size_t)flips[(t * 4 + f) % 4096] << 8 |
                                 flips[(t * 4 + f + 1) % 4096]) * 2654435761u % z.len;
                    copy[at] ^= (unsigned char)(1u << ((t + f) % 8));
                }
                decode(codec, copy, z.len, &d);
                flips_done++;
            }
            /* Random garbage of assorted lengths, with the right magic. */
            for (int t = 0; t < 200; t++) {
                size_t len = 1 + (size_t)t * 13 % 600;
                unsigned char junk[600];
                fill_random(junk, len, (unsigned)t + 1000);
                if (codec->magic && len >= codec->magic_len)
                    memcpy(junk, codec->magic, codec->magic_len);
                decode(codec, junk, len, &d);
            }
            free(copy);
            sq_buf_free(&z);
            sq_buf_free(&d);
        }
        CHECK(truncations_ok);
        if (!truncations_ok)
            fprintf(stderr, "  %s: a truncated stream decoded as complete\n", codec->name);
        CHECK(flips_done > 0);
    }
    TEST_DONE("robust");
}
