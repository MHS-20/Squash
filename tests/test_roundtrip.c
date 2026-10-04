/* Every codec, every level, compress then decompress our own output. */
#include "codec.h"
#include "corpus.h"

int main(int argc, char **argv)
{
    /* argv[1] limits the run to one codec, handy when debugging. */
    const char *only = argc > 1 ? argv[1] : NULL;
    sample s[40];
    size_t ns = corpus_build(s, 40, 1);
    for (size_t c = 0; c < sq_codec_count; c++) {
        const sq_codec *codec = &sq_codecs[c];
        if (only && strcmp(only, codec->name))
            continue;
        for (int level = codec->min_level; level <= codec->max_level; level++) {
            for (size_t i = 0; i < ns; i++) {
                /* Every level on the small samples, default on big ones. */
                if (s[i].len > 300000 && level != codec->max_level)
                    continue;
                sq_buf z, d;
                sq_buf_init(&z);
                sq_buf_init(&d);
                int e1 = codec->compress(s[i].data, s[i].len, level, &z);
                int e2 = e1 ? e1 : codec->decompress(z.data, z.len, &d);
                int ok = !e1 && !e2 && same_bytes(d.data, d.len, s[i].data, s[i].len);
                CHECK(ok);
                if (!ok)
                    fprintf(stderr, "  %s level %d on %s: %s / %s\n", codec->name, level,
                            s[i].name, sq_strerror(e1), sq_strerror(e2));
                sq_buf_free(&z);
                sq_buf_free(&d);
            }
        }
    }
    corpus_free(s, ns);
    TEST_DONE("roundtrip");
}
